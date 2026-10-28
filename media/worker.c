/* SPDX-License-Identifier: Apache-2.0 */
/* Separate C process: the Android C2 service never loads old platform C++ DSOs. */
#define _GNU_SOURCE
#include "worker_protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define SLOTS 37
#define LIBDIR "/vendor/lib/mt6580-codec"
struct val_memory {
    uint32_t signature, type;
    uint64_t size, va, iova;
    uint32_t alignment, pad;
    uint64_t aligned_va, aligned_iova;
    uint32_t codec, share_fd;
    uint64_t ion_handle, reserved, reserved_size;
    uint32_t ion_fd, pad_end;
};
struct descriptor { _Alignas(8) uint32_t word[34]; };
_Static_assert(sizeof(struct val_memory) == 96, "VAL ABI");
_Static_assert(sizeof(struct descriptor) == 0x88, "frame ABI");
struct slot {
    struct val_memory memory;
    struct descriptor desc;
    uint64_t token;
    int64_t pts;
    int busy;
};
struct api {
    int (*create)(void **, uint32_t);
    int (*init)(void *, void *, void *);
    int (*decode)(void *, void *, void *);
    int (*get)(void *, uint32_t, void *, void *);
    int (*set)(void *, uint32_t, void *);
    int (*deinit)(void *);
    int (*release)(void *);
    int (*alloc)(struct val_memory *, uint32_t);
    int (*free)(struct val_memory *, uint32_t);
    int (*flush)(struct val_memory *, uint32_t, uint32_t);
    int (*invalidate)(struct val_memory *, uint32_t, uint32_t);
} api;
struct session {
    int fd;
    uint32_t generation, width, height, stride, chroma_stride, lines, order;
    uint8_t csd[MT6580_C2_MAX_CSD_BYTES];
    size_t csd_bytes;
    void *instance;
    int initialized, draining;
    struct val_memory csd_memory;
    struct descriptor csd_desc;
    struct slot in[SLOTS], out[SLOTS];
};
struct nal { const uint8_t *data; size_t size, prefix; uint8_t type; };

static int transfer(int fd, void *memory, size_t bytes, int sending)
{
    uint8_t *p = memory;
    while (bytes) {
        ssize_t n = sending ? send(fd, p, bytes, MSG_NOSIGNAL) : recv(fd, p, bytes, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n ? errno : EPIPE;
        p += n; bytes -= n;
    }
    return 0;
}
static int reply(struct session *s, uint32_t command, uint64_t token, int64_t pts,
                 uint32_t flags, const void *payload, size_t bytes)
{
    struct mt6580_c2_header h = {MT6580_C2_MAGIC, MT6580_C2_VERSION, command,
        (uint32_t)bytes, token, pts, s->generation, flags};
    int error = transfer(s->fd, &h, sizeof(h), 1);
    return error || !bytes ? error : transfer(s->fd, (void *)payload, bytes, 1);
}
static size_t prefix(const uint8_t *p, size_t size)
{
    if (size >= 4 && !memcmp(p, "\0\0\0\1", 4)) return 4;
    if (size >= 3 && !memcmp(p, "\0\0\1", 3)) return 3;
    return 0;
}
/* Reject leading junk and empty NALs; Annex-B framing never dereferences past size. */
static int next_nal(const uint8_t **p, size_t *bytes, struct nal *n)
{
    if (!*bytes) return 0;
    size_t start = prefix(*p, *bytes), end;
    if (!start || *bytes <= start || ((*p)[start] & 0x80)) return -EINVAL;
    for (end = start + 1; end < *bytes && !prefix(*p + end, *bytes - end); ++end) {}
    *n = (struct nal){*p, end, start, (*p)[start] & 31};
    *p += end; *bytes -= end;
    return 1;
}
struct bits { const uint8_t *p; size_t count, pos; int error; };
static uint32_t bits(struct bits *b, unsigned count)
{
    uint32_t value = 0;
    if (count > 32 || b->pos + count > b->count) { b->error = 1; return 0; }
    while (count--) { value = (value << 1) | ((b->p[b->pos / 8] >> (7 - b->pos % 8)) & 1); b->pos++; }
    return value;
}
static uint32_t ue(struct bits *b)
{
    unsigned zeros = 0;
    while (!b->error && !bits(b, 1)) if (++zeros > 30) { b->error = 1; return 0; }
    return b->error ? 0 : ((UINT32_C(1) << zeros) - 1) + bits(b, zeros);
}
static int32_t se(struct bits *b)
{
    uint32_t value = ue(b);
    return (value & 1) ? (int32_t)((value + 1) / 2) : -(int32_t)(value / 2);
}
static int scaling_list(struct bits *b, unsigned size)
{
    /* H.264 7.3.2.1.1: each present list has at most 64 signed deltas. */
    int last = 8, next = 8;
    for (unsigned i = 0; i < size; ++i) {
        if (next) {
            int32_t delta = se(b);
            if (b->error || delta < -128 || delta > 127) return EINVAL;
            next = (last + delta + 256) % 256;
        }
        if (next) last = next;
    }
    return 0;
}
/* Bound dimensions and the High-profile extension before an old binary can
 * allocate from untrusted SPS. Only progressive, 8-bit 4:2:0 is admitted.
 * Accepting a syntax here does not advertise that profile in the C2 interface. */
static int validate_sps(const struct nal *n, uint32_t *width, uint32_t *height)
{
    uint8_t rbsp[MT6580_C2_MAX_CSD_BYTES];
    size_t len = 0;
    unsigned zeros = 0;
    if (!n || !width || !height || n->type != 7 ||
        n->prefix >= n->size || n->size > sizeof(rbsp)) return EINVAL;
    for (size_t i = n->prefix + 1; i < n->size; ++i) {
        uint8_t v = n->data[i];
        if (zeros >= 2 && v == 3) {
            if (i + 1 == n->size || n->data[i + 1] > 3) return EINVAL;
            zeros = 0; continue;
        }
        if (len == sizeof(rbsp)) return EINVAL;
        rbsp[len++] = v; zeros = v ? 0 : zeros + 1;
    }
    struct bits b = {rbsp, len * 8, 0, 0};
    uint32_t profile = bits(&b, 8);
    if (b.error) return EINVAL;
    if (profile != 66 && profile != 77 && profile != 100) return ENOTSUP;
    (void)bits(&b, 8); (void)bits(&b, 8);
    if (ue(&b) > 31 || b.error) return EINVAL;
    if (profile == 100) {
        /* Reject monochrome, 4:2:2/4:4:4, high bit depths and transform
         * bypass: the native output copy is specifically 8-bit planar 420. */
        uint32_t chroma = ue(&b), luma_depth = ue(&b), chroma_depth = ue(&b);
        uint32_t bypass = bits(&b, 1);
        if (b.error) return EINVAL;
        if (chroma != 1 || luma_depth || chroma_depth || bypass) return ENOTSUP;
        if (bits(&b, 1)) {
            for (unsigned i = 0; i < 8; ++i) {
                if (bits(&b, 1) && scaling_list(&b, i < 6 ? 16 : 64)) return EINVAL;
            }
        }
        if (b.error) return EINVAL;
    }
    if (ue(&b) > 12 || b.error) return EINVAL;
    uint32_t poc = ue(&b);
    if (poc == 0) { if (ue(&b) > 12) return EINVAL; }
    else if (poc == 1) {
        (void)bits(&b, 1); (void)se(&b); (void)se(&b);
        uint32_t cycle = ue(&b);
        if (cycle > 255) return EINVAL;
        for (uint32_t i = 0; i < cycle; ++i) (void)se(&b);
    } else if (poc != 2) return EINVAL;
    if (ue(&b) > 16) return EINVAL;
    (void)bits(&b, 1);
    uint32_t mbw = ue(&b), mbh = ue(&b), progressive = bits(&b, 1);
    if (b.error) return EINVAL;
    if (mbw >= MT6580_C2_MAX_WIDTH / 16 || mbh >= MT6580_C2_MAX_HEIGHT / 16 ||
        !progressive) return ENOTSUP;
    (void)bits(&b, 1);
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (bits(&b, 1)) { left = ue(&b); right = ue(&b); top = ue(&b); bottom = ue(&b); }
    uint32_t w = (mbw + 1) * 16, h = (mbh + 1) * 16;
    if (b.error || left > w / 2 || right > w / 2 || top > h / 2 || bottom > h / 2 ||
        left + right >= w / 2 || top + bottom >= h / 2) return EINVAL;
    w -= 2 * (left + right); h -= 2 * (top + bottom);
    /* Left/top crop needs plane offsets and is not silently misrepresented. */
    if (left || top || w < 16 || h < 16) return ENOTSUP;
    *width = w; *height = h;
    return 0;
}
static int allocate(struct val_memory *m, size_t bytes)
{
    if (m->va && m->size >= bytes) return 0;
    if (m->va && api.free(m, sizeof(*m))) return EIO;
    memset(m, 0, sizeof(*m));
    m->type = 3; m->codec = 1; m->size = bytes; m->alignment = 4096;
    if (api.alloc(m, sizeof(*m)) || !m->va || !m->iova ||
        m->va > UINT32_MAX || m->iova > UINT32_MAX || (m->va & 4095) || (m->iova & 4095)) return ENOMEM;
    return 0;
}
static void describe(struct descriptor *d, struct val_memory *m, size_t bytes, int input)
{
    memset(d, 0, sizeof(*d));
    d->word[0] = (uint32_t)m->va; d->word[2] = (uint32_t)m->iova; d->word[4] = bytes;
    if (input) { d->word[6] = (uint32_t)m->va; d->word[8] = (uint32_t)m->va + bytes; }
}
static void cleanup(struct session *s)
{
    /* Driver retains descriptor addresses and DPB memory until DeInit. */
    if (s->initialized) api.deinit(s->instance);
    if (s->instance) api.release(s->instance);
    s->instance = NULL; s->initialized = 0;
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (s->in[i].memory.va) api.free(&s->in[i].memory, sizeof(struct val_memory));
        if (s->out[i].memory.va) api.free(&s->out[i].memory, sizeof(struct val_memory));
    }
    if (s->csd_memory.va) api.free(&s->csd_memory, sizeof(struct val_memory));
    memset(&s->csd_memory, 0, sizeof(s->csd_memory));
    memset(s->in, 0, sizeof(s->in)); memset(s->out, 0, sizeof(s->out));
    s->draining = 0;
}
static int reap(struct session *s)
{
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (unsigned n = 0; n < SLOTS * 2; ++n) {
            uint32_t ptr = 0;
            if (api.get(s->instance, kind ? 3 : 0x15, NULL, &ptr)) return EIO;
            if (!ptr) break;
            struct slot *pool = kind ? s->out : s->in;
            unsigned i;
            for (i = 0; i < SLOTS && ptr != (uintptr_t)&pool[i].desc; ++i) {}
            if (i == SLOTS || !pool[i].busy) return EPROTO;
            pool[i].busy = 0;
            if (n == SLOTS * 2 - 1) return ELOOP;
        }
    }
    return 0;
}
static int display(struct session *s, int discard)
{
    for (unsigned n = 0; n < SLOTS * 2; ++n) {
        uint32_t ptr = 0;
        if (api.get(s->instance, 2, NULL, &ptr)) return EIO;
        if (!ptr) return reap(s);
        unsigned i;
        for (i = 0; i < SLOTS && ptr != (uintptr_t)&s->out[i].desc; ++i) {}
        if (i == SLOTS || !s->out[i].busy) return EPROTO;
        struct slot *slot = &s->out[i];
        if (discard || (slot->desc.word[22] & 7)) continue;
        if (api.invalidate(&slot->memory, sizeof(slot->memory), 1)) return EIO;
        size_t bytes = s->width * s->height * 3 / 2;
        uint8_t *packet = malloc(sizeof(struct mt6580_c2_frame_info) + bytes);
        if (!packet) return ENOMEM;
        struct mt6580_c2_frame_info info = {s->width, s->height, bytes, 0};
        memcpy(packet, &info, sizeof(info));
        uint8_t *dst = packet + sizeof(info), *y = (uint8_t *)(uintptr_t)slot->memory.va;
        size_t ys = s->stride * s->lines, cs = s->chroma_stride * s->lines / 2;
        uint8_t *u = y + ys, *v = u + cs;
        if (s->order) { uint8_t *tmp = u; u = v; v = tmp; }
        for (unsigned r = 0; r < s->height; ++r) memcpy(dst + r * s->width, y + r * s->stride, s->width);
        for (unsigned r = 0; r < s->height / 2; ++r) {
            memcpy(dst + s->width * s->height + r * s->width / 2, u + r * s->chroma_stride, s->width / 2);
            memcpy(dst + s->width * s->height * 5 / 4 + r * s->width / 2, v + r * s->chroma_stride, s->width / 2);
        }
        int error = reply(s, MT6580_C2_FRAME, slot->token, slot->pts, 0, packet, sizeof(info) + bytes);
        free(packet);
        if (error) return error;
    }
    return ELOOP;
}
static int submit(struct session *s, const uint8_t *bytes, size_t count,
                  uint64_t token, int64_t pts, int config)
{
    unsigned in, out;
    int error = reap(s);
    if (error) return error;
    for (in = 0; in < SLOTS && s->in[in].busy; ++in) {}
    for (out = 0; out < SLOTS && s->out[out].busy; ++out) {}
    if (in == SLOTS || out == SLOTS) return ENOBUFS;
    struct slot *input = &s->in[in], *frame = &s->out[out];
    size_t span = s->lines * (s->stride + s->chroma_stride);
    if ((error = allocate(&input->memory, count + 64)) ||
        (error = allocate(&frame->memory, span + 4096))) return error;
    memcpy((void *)(uintptr_t)input->memory.va, bytes, count);
    memset((uint8_t *)(uintptr_t)input->memory.va + count, 0, 64);
    describe(&input->desc, &input->memory, count, 1);
    describe(&frame->desc, &frame->memory, span, 0);
    frame->token = token; frame->pts = pts;
    frame->desc.word[17] = (uint32_t)token;
    if (api.flush(&input->memory, sizeof(input->memory), 1) ||
        api.flush(&frame->memory, sizeof(frame->memory), 1)) return EIO;
    input->busy = frame->busy = 1;
    int rc = api.decode(s->instance, &input->desc, &frame->desc);
    /* A single access unit containing slices is passed as one descriptor.
     * More-output-buffer and resolution-change are not guessed as success. */
    if (rc) { fprintf(stderr, "mt6580 codec: Decode rc=%d\n", rc); return ENOTSUP; }
    return display(s, config);
}
static int initialize(struct session *s)
{
    const uint8_t *p = s->csd;
    size_t bytes = s->csd_bytes;
    struct nal n;
    int seen_sps = 0, seen_pps = 0, rc;
    while ((rc = next_nal(&p, &bytes, &n)) > 0) {
        if (n.type == 7) {
            int error = validate_sps(&n, &s->width, &s->height);
            if (error) return error;
            seen_sps = 1;
        } else if (n.type == 8) seen_pps = 1;
        else return EINVAL;
    }
    if (rc < 0 || !seen_sps || !seen_pps) return EINVAL;
    int error = allocate(&s->csd_memory, s->csd_bytes + 64);
    if (error) return error;
    memcpy((void *)(uintptr_t)s->csd_memory.va, s->csd, s->csd_bytes);
    memset((uint8_t *)(uintptr_t)s->csd_memory.va + s->csd_bytes, 0, 64);
    describe(&s->csd_desc, &s->csd_memory, s->csd_bytes, 1);
    if (api.flush(&s->csd_memory, sizeof(s->csd_memory), 1)) return EIO;
    if (api.create(&s->instance, 0x100) || !s->instance) return EIO;
    long tid = syscall(__NR_gettid);
    if (tid <= 0 || tid > UINT16_MAX) return EOVERFLOW;
    uint32_t tids = (uint32_t)tid << 16 | (uint32_t)tid;
    if (api.set(s->instance, 0xf, &tids)) return EIO;
    uint32_t picture[64] = {0};
    if (api.init(s->instance, &s->csd_desc, picture)) return EIO;
    s->initialized = 1;
    if (picture[0] != s->width || picture[1] != s->height) return EPROTO;
    uint32_t *codec = (uint32_t *)(uintptr_t)((uint32_t *)s->instance)[11];
    if (!codec) return EPROTO;
    s->stride = codec[600]; s->chroma_stride = codec[601];
    s->lines = (codec[585] + 15) & ~15u; s->order = codec[602];
    if (s->stride < s->width || s->stride > 2048 || s->chroma_stride < s->width / 2 ||
        s->chroma_stride > 1024 || s->lines < s->height || s->lines > 736 || s->order > 1) return ENOTSUP;
    /* Init's SD parser does not configure the actual XA hardware decoder. */
    p = s->csd; bytes = s->csd_bytes;
    while (next_nal(&p, &bytes, &n) > 0) {
        error = submit(s, n.data, n.size, 0, 0, 1);
        if (error) return error;
    }
    struct mt6580_c2_format format = {s->width, s->height};
    return reply(s, MT6580_C2_FORMAT, 0, 0, 0, &format, sizeof(format));
}
static int access_unit(struct session *s, uint8_t *payload, size_t count,
                       uint64_t token, int64_t pts)
{
    if (s->draining) return EPIPE;
    const uint8_t *p = payload;
    size_t bytes = count;
    struct nal n;
    unsigned vcl = 0;
    /* Remove delimiters/SEI; preserve all VCL slices of the access unit. */
    size_t written = 0;
    int rc;
    while ((rc = next_nal(&p, &bytes, &n)) > 0) {
        if (n.type == 1 || n.type == 5) {
            memmove(payload + written, n.data, n.size); written += n.size; vcl++;
        } else if (n.type == 7 || n.type == 8) {
            if (s->initialized) return ENOTSUP;
            if (s->csd_bytes + n.size > sizeof(s->csd)) return E2BIG;
            memcpy(s->csd + s->csd_bytes, n.data, n.size); s->csd_bytes += n.size;
        } else if (n.type != 6 && n.type != 9 && n.type != 12) return ENOTSUP;
    }
    if (rc < 0 || !vcl) return EINVAL;
    if (!s->initialized) {
        int error = initialize(s);
        if (error) return error;
    }
    return submit(s, payload, written, token, pts, 0);
}
static void serve(int fd)
{
    struct session *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->fd = fd;
    for (;;) {
        struct mt6580_c2_header h;
        /* Pausing playback must not invalidate the decoder after 35 seconds.
         * Payload reads/writes remain bounded once a request has begun. */
        struct timeval idle = {0, 0}, active = {35, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle));
        int error = transfer(fd, &h, sizeof(h), 0);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &active, sizeof(active));
        if (error) break;
        if (h.magic != MT6580_C2_MAGIC || h.version != MT6580_C2_VERSION || !h.generation ||
            (h.flags & ~MT6580_C2_FLAG_EOS) || (h.command != MT6580_C2_DRAIN && h.flags) ||
            h.payload_bytes > MT6580_C2_MAX_AU_BYTES ||
            (s->generation && s->generation != h.generation && h.command != MT6580_C2_RESET)) break;
        s->generation = h.generation;
        uint8_t *payload = h.payload_bytes ? malloc(h.payload_bytes) : NULL;
        if (h.payload_bytes && !payload) break;
        if (h.payload_bytes && transfer(fd, payload, h.payload_bytes, 0)) { free(payload); break; }
        uint32_t response = MT6580_C2_ACK;
        switch (h.command) {
        case MT6580_C2_CONFIG:
            if (s->initialized || !h.payload_bytes || h.payload_bytes > sizeof(s->csd) - s->csd_bytes) error = EINVAL;
            else { memcpy(s->csd + s->csd_bytes, payload, h.payload_bytes); s->csd_bytes += h.payload_bytes; }
            break;
        case MT6580_C2_AU:
            error = h.payload_bytes ? access_unit(s, payload, h.payload_bytes, h.token, h.pts) : EINVAL;
            break;
        case MT6580_C2_DRAIN:
            if (h.payload_bytes) { error = EINVAL; break; }
            if (s->initialized) {
                error = api.decode(s->instance, NULL, NULL) ? EIO : display(s, 0);
                s->draining = 1;
            }
            response = MT6580_C2_DRAINED;
            break;
        case MT6580_C2_RESET:
            if (h.payload_bytes) { error = EINVAL; break; }
            cleanup(s); s->csd_bytes = 0;
            break;
        case MT6580_C2_CLOSE:
            if (h.payload_bytes) error = EINVAL;
            break;
        default: error = ENOTSUP;
        }
        free(payload);
        if (error) {
            int32_t status = error;
            (void)reply(s, MT6580_C2_ERROR, h.token, h.pts, 0, &status, sizeof(status));
            break;
        }
        if (reply(s, response, h.token, h.pts,
                  response == MT6580_C2_DRAINED ? h.flags : 0, NULL, 0)) break;
        if (h.command == MT6580_C2_CLOSE) break;
    }
    cleanup(s); free(s);
}
static int load_api(void)
{
    const char *directory = getenv("MT6580_CODEC_LIBDIR");
    char name[512];
    if (!directory) directory = LIBDIR;
    if (snprintf(name, sizeof(name), "%s/libvcodecdrv.so", directory) >= (int)sizeof(name)) return E2BIG;
    void *lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "mt6580 codec: %s\n", dlerror()); return ENOENT; }
#define LOAD(field, source, symbol) do { *(void **)(&api.field) = dlsym(source, symbol); if (!api.field) return ENOENT; } while (0)
    LOAD(create, lib, "eVDecDrvCreate"); LOAD(init, lib, "eVDecDrvInit");
    LOAD(decode, lib, "eVDecDrvDecode"); LOAD(get, lib, "eVDecDrvGetParam");
    LOAD(set, lib, "eVDecDrvSetParam"); LOAD(deinit, lib, "eVDecDrvDeInit");
    LOAD(release, lib, "eVDecDrvRelease");
    LOAD(alloc, RTLD_DEFAULT, "eVideoMemAlloc"); LOAD(free, RTLD_DEFAULT, "eVideoMemFree");
    LOAD(flush, RTLD_DEFAULT, "eVideoFlushCache"); LOAD(invalidate, RTLD_DEFAULT, "eVideoInvalidateCache");
#undef LOAD
    return 0;
}
int main(int argc, char **argv)
{
    if (sizeof(void *) != 4) return 2;
    signal(SIGPIPE, SIG_IGN);
    if (load_api()) return 1;
    int server = -1;
    const char *inherited = getenv("ANDROID_SOCKET_mt6580_vcodec");
    if (inherited) {
        char *end; long n = strtol(inherited, &end, 10);
        if (*end || n < 0 || n > INT32_MAX) return 2;
        server = (int)n;
        /* init normalizes punctuation in the environment key. Verify the
         * inherited descriptor is actually our named Unix stream socket. */
        struct sockaddr_un address = {0};
        socklen_t bytes = sizeof(address), type_bytes = sizeof(int);
        int type = 0;
        if (getsockname(server, (struct sockaddr *)&address, &bytes) ||
            address.sun_family != AF_UNIX ||
            !memchr(address.sun_path, 0, sizeof(address.sun_path)) ||
            strcmp(address.sun_path, MT6580_C2_SOCKET) ||
            getsockopt(server, SOL_SOCKET, SO_TYPE, &type, &type_bytes) ||
            type != SOCK_STREAM) return 2;
    } else if (argc == 3 && !strcmp(argv[1], "--socket") && getuid() == 0) {
        /* Private bring-up only; production always uses init's owned socket. */
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        if (strlen(argv[2]) >= sizeof(addr.sun_path)) return 2;
        strcpy(addr.sun_path, argv[2]);
        server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr))) return 1;
    } else return 2;
    if (listen(server, 1)) return 1;
    for (;;) {
        int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; return 1; }
        struct ucred cred;
        socklen_t size = sizeof(cred);
        /* mediacodec plus root for explicit local qualification. */
        if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) &&
            (cred.uid == 1046 || cred.uid == 0)) {
            struct timeval timeout = {35, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            serve(fd);
        }
        close(fd);
    }
}
