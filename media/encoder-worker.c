/* SPDX-License-Identifier: Apache-2.0 */
/* Isolated owner-supplied MPEG4/H263 hybrid backend; never labels stock AVC
 * software encoding as hardware. Native type/IRQ support is a prerequisite.
 *
 * The recovered wrapper exposes delayed FIFO output, not a returned timestamp.
 * This implementation admits no B frames and retains input descriptors until
 * their FIFO packet is actually returned. Native probe qualification must still
 * confirm rate-control/drop behavior and long-run reuse before a C2 component
 * advertises the stream. Unexpected empty final output or too much delay fails;
 * it is not hidden by assigning a packet the current request's token.
 */
#define _GNU_SOURCE
#include "encoder_protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define LIBDIR "/vendor/lib/mt6580-codec"
#define SLOTS 8
#define OWNED_LIMIT (UINT64_C(64) * 1024 * 1024)
struct val_memory {
    uint32_t signature, type;
    uint64_t size, va, iova;
    uint32_t alignment, pad;
    uint64_t aligned_va, aligned_iova;
    uint32_t codec, share_fd;
    uint64_t ion_handle, reserved, reserved_size;
    uint32_t ion_fd, pad_end;
};
struct frame_desc { _Alignas(8) uint32_t word[22]; };
struct bs_desc { _Alignas(8) uint32_t word[18]; };
struct done_result { _Alignas(8) uint32_t word[10]; };
_Static_assert(sizeof(struct val_memory) == 96, "VAL ABI");
_Static_assert(sizeof(struct frame_desc) == 88, "encoder frame ABI");
_Static_assert(sizeof(struct bs_desc) == 72, "encoder bitstream ABI");
struct slot {
    struct val_memory memory;
    struct frame_desc desc;
    uint64_t token;
    int64_t pts;
    int busy;
};
struct api {
    int (*create)(void **, uint32_t);
    int (*init)(void *);
    int (*set)(void *, uint32_t, void *, void *);
    int (*encode)(void *, uint32_t, void *, void *, void *);
    int (*encode_one_unit)(void *, void *, void *);
    int (*encode_open)(void *, void *, void *, void *);
    int (*deinit)(void *);
    int (*release)(void *);
    int (*alloc)(struct val_memory *, uint32_t);
    int (*free)(struct val_memory *, uint32_t);
    int (*flush)(struct val_memory *, uint32_t, uint32_t);
    int (*invalidate)(struct val_memory *, uint32_t, uint32_t);
} api;
struct session {
    int fd;
    uint32_t generation, frame_bytes;
    struct mt6580_enc_config config;
    void *instance;
    int initialized, configuring, header_attempted, opened, draining;
    struct val_memory bitstream;
    struct bs_desc bs;
    struct done_result done;
    struct slot in[SLOTS];
    unsigned queue[SLOTS], head, count;
    /* SetParam copies these fields today, but keeping them stable also avoids
     * accidental lifetime dependence on a stack frame during ABI bring-up. */
    uint32_t setting[39], extra[8];
    uint32_t *hal, *original_api;
    uint32_t shadow_api[11];
    int open_seen, level_applied;
};
/* The old wrapper silently drops its public profile/level fields. Its
 * recovered per-instance wrapper API is eleven u32 pointers at driver+0x300.
 * EncMp4SeqHeader invokes Open(HAL, original_API, callbacks=HAL+2, out=HAL),
 * then ordinary SetParameter/Init/GenerateHeader. Shadow only the dispatch
 * table; leave the original writable parameter block and original backend
 * table intact. The shadow survives until actual DeInit and Release finish.
 *
 * The Open hook delegates the exact four-argument EncodeOpen ABI first,
 * obtaining the real 0x490-byte MPEG4 codec object. It then uses the backend's
 * supported SetParameter PROFILE/LEVEL API before Init/GenerateHeader.
 * MPEG4EncoderGenerateHeader maps profile0/level0 -> VOS08, level8 -> VOS06;
 * cfg[2] alone never reaches these setters. No output bytes are rewritten.
 */
static __thread struct session *opening_session;
static int encoder_open_adapter(void *owner, void *parameters, void *callbacks, void *out)
{
    struct session *s = opening_session;
    if (!s || !s->configuring || s->open_seen || owner != s->hal ||
        parameters != s->original_api || callbacks != s->hal + 2 || out != s->hal)
        return 2;
    s->open_seen = 1;
    int result = api.encode_open(owner, parameters, callbacks, out);
    if (result || !s->hal[0]) return result ? result : 2;
    if (s->config.codec == 1) {
        int (*set_parameter)(void *, uint32_t, void *) =
            (void *)(uintptr_t)s->original_api[4];
        uint32_t profile = 0, level = s->config.level == 6 ? 8 : 0;
        if (set_parameter((void *)(uintptr_t)s->hal[0], 0x29, &profile) ||
            set_parameter((void *)(uintptr_t)s->hal[0], 0x2a, &level)) return 2;
        fprintf(stderr, "VENC_LEVEL simple=%u backend_enum=%u\n", s->config.level, level);
    }
    s->level_applied = 1;
    return 0;
}
static int install_open_adapter(struct session *s)
{
    uint32_t *driver = s->instance;
    uint32_t *context = (uint32_t *)(uintptr_t)driver[6];
    if (!context || context[0] != s->config.codec) return EPROTO;
    uint32_t *hal = (uint32_t *)(uintptr_t)context[2];
    uint32_t *original = driver + 0xc0;
    if ((uintptr_t)hal != (uintptr_t)driver + 0xf0 ||
        hal[0xb] != (uintptr_t)original ||
        original[1] != (uintptr_t)api.encode_open ||
        original[6] != (uintptr_t)api.encode_one_unit ||
        !original[4] || !original[9] || !original[10]) return EPROTO;
    uint32_t *backend = (uint32_t *)(uintptr_t)original[10];
    if (original[4] != backend[4] || original[9] != backend[9]) return EPROTO;
    s->hal = hal; s->original_api = original;
    memcpy(s->shadow_api, original, sizeof(s->shadow_api));
    s->shadow_api[1] = (uint32_t)(uintptr_t)encoder_open_adapter;
    hal[0xb] = (uint32_t)(uintptr_t)s->shadow_api;
    return 0;
}
static int restore_api(struct session *s)
{
    if (!s->original_api) return 0;
    if (!s->hal || s->hal[0xb] != (uintptr_t)s->shadow_api ||
        s->original_api != (uint32_t *)s->instance + 0xc0) return EPROTO;
    s->hal[0xb] = (uint32_t)(uintptr_t)s->original_api;
    s->hal = s->original_api = NULL;
    return 0;
}
static int transfer(int fd, void *memory, size_t bytes, int sending)
{
    uint8_t *p = memory;
    const size_t requested = bytes;
    while (bytes) {
        ssize_t n = sending ? send(fd, p, bytes, MSG_NOSIGNAL) : recv(fd, p, bytes, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int error = n ? errno : EPIPE;
            fprintf(stderr, "VENC_TRANSFER sending=%d requested=%zu remaining=%zu error=%d\n",
                    sending, requested, bytes, error);
            return error;
        }
        p += n; bytes -= n;
    }
    return 0;
}
static int reply(struct session *s, uint32_t command, uint64_t token, int64_t pts,
                 uint32_t flags, const void *payload, size_t bytes)
{
    if (bytes > MT6580_ENC_MAX_PACKET_BYTES || (bytes && !payload)) return EINVAL;
    struct mt6580_c2_header h = {MT6580_ENC_MAGIC, MT6580_ENC_VERSION, command,
        (uint32_t)bytes, token, pts, s->generation, flags};
    int error = transfer(s->fd, &h, sizeof(h), 1);
    return error || !bytes ? error : transfer(s->fd, (void *)payload, bytes, 1);
}
static int allocate(struct val_memory *m, size_t bytes)
{
    if (!bytes || bytes > OWNED_LIMIT) return EINVAL;
    memset(m, 0, sizeof(*m));
    m->type = 3; m->codec = 1; m->size = bytes; m->alignment = 4096;
    if (api.alloc(m, sizeof(*m))) return ENOMEM;
    if (!m->va || !m->iova || m->va > UINT32_MAX || m->iova > UINT32_MAX ||
        m->size < bytes || (m->va & 4095) || (m->iova & 4095)) return EPROTO;
    return 0;
}
static void prepare_output(struct session *s)
{
    memset(&s->bs, 0, sizeof(s->bs));
    s->bs.word[0] = (uint32_t)s->bitstream.va;
    s->bs.word[2] = (uint32_t)s->bitstream.iova;
    s->bs.word[4] = MT6580_ENC_MAX_PACKET_BYTES;
    memset(&s->done, 0, sizeof(s->done));
    s->done.word[0] = 1;
}
static int output_bytes(struct session *s, uint8_t **data, uint32_t *bytes)
{
    uint64_t address = s->bs.word[0], n = s->bs.word[8];
    if (s->bs.word[1] || s->bs.word[9] || address < s->bitstream.va ||
        address - s->bitstream.va > s->bitstream.size ||
        n > s->bitstream.size - (address - s->bitstream.va) ||
        n > MT6580_ENC_MAX_PACKET_BYTES) return EOVERFLOW;
    if (n && api.invalidate(&s->bitstream, sizeof(s->bitstream), 1)) return EIO;
    *data = (uint8_t *)(uintptr_t)address; *bytes = (uint32_t)n;
    return 0;
}
static int packet(struct session *s, int emit, int *produced)
{
    uint8_t *data;
    uint32_t bytes;
    int error = output_bytes(s, &data, &bytes);
    *produced = 0;
    if (error || !bytes) return error;
    if (!s->count || s->done.word[2] != (uintptr_t)&s->bs ||
        s->done.word[3] || (s->done.word[6] & 255) > 1) return EPROTO;
    unsigned i = s->queue[s->head];
    struct slot *oldest = &s->in[i];
    if (!oldest->busy) return EPROTO;
    uint32_t flags = (s->done.word[6] & 255) ? MT6580_ENC_FLAG_KEY_FRAME : 0;
    if (emit) error = reply(s, MT6580_ENC_PACKET, oldest->token, oldest->pts,
                            flags, data, bytes);
    /* The stock wrapper has completed/copy-returned this FIFO frame. Only now
     * is this slot available to be overwritten by a later request. */
    oldest->busy = 0;
    s->head = (s->head + 1) % SLOTS; s->count--;
    *produced = 1;
    return error;
}
/* The stock EncMp4Frame_hybrid(FINAL) fails to initialize the raw input
 * fields that EncodeOneUnit unconditionally reads (notably +0x68, stride
 * pointer). Its success depends on stale caller-stack bytes. Do not invoke
 * that broken shim or try to preserve its previous stack contents.
 *
 * Recovered interfaces, pinned to the owner DSO by the packaging hash:
 * driver+0x18 -> hybrid context, context+8 -> embedded HAL at driver+0xf0;
 * HAL+0x2c -> wrapper API at driver+0x300. The wrapper EncodeOneUnit accepts
 * 27 u32 words. Populate its full source, ring-buffer and geometry ABI from
 * retained, validated state, then call the real backend FINAL through the
 * already registered wrapper API. GetNextBitstream returns ten u32 words.
 * Evidence: EncodeOneUnit 0x1bfb0/0x1bfba, EncMp4Frame_hybrid 0x33af0,
 * MPEG4EncoderEncodeOneFrame 0x1b088 and GetNextBitstream 0x1a58c.
 */
static int encode_final(struct session *s)
{
    if (!s->instance || !s->count) return EINVAL;
    uint32_t *driver = s->instance;
    uint32_t *context = (uint32_t *)(uintptr_t)driver[6];
    if (!context || context[0] != s->config.codec) return EPROTO;
    uint32_t *hal = (uint32_t *)(uintptr_t)context[2];
    if ((uintptr_t)hal != (uintptr_t)driver + 0xf0 || !hal[0]) return EPROTO;
    uint32_t *table = (uint32_t *)(uintptr_t)hal[0xb];
    if (hal != s->hal || table != s->shadow_api ||
        s->original_api != driver + 0xc0 ||
        table[6] != (uintptr_t)api.encode_one_unit || !table[9] ||
        hal[0x3e] != 0 || hal[0x34] != s->config.width ||
        hal[0x35] != s->config.height || hal[0x43] != s->config.width ||
        hal[0x44] != s->config.height) return EPROTO;
    /* MPEG4's actual 2 MiB temporary bitstream allocation, split into two
     * 1 MiB pipeline halves, is retained in these HAL fields by the normal
     * frame path. Validate every ring pointer before passing it onward. */
    uint64_t base = hal[0x62], iova = hal[100], length = hal[0x5a];
    if (!base || !iova || length != 0x200000 || hal[0x21] != length / 2 ||
        base + length > UINT32_MAX || iova + length > UINT32_MAX) return EPROTO;
    const unsigned pair[] = {0x15, 0x18, 0x1b, 0x1e};
    for (unsigned i = 0; i < sizeof(pair) / sizeof(pair[0]); ++i) {
        unsigned j = pair[i];
        uint64_t address = hal[j], bus = hal[j + 1];
        if (address < base || address > base + length ||
            bus != iova + address - base || hal[j + 2] != 1) return EPROTO;
    }
    if ((uint64_t)hal[0x18] - hal[0x15] != length / 2 ||
        (uint64_t)hal[0x19] - hal[0x16] != length / 2) return EPROTO;
    struct slot *oldest = &s->in[s->queue[s->head]];
    if (!oldest->busy) return EPROTO;
    uint32_t raw[27] = {0};
    raw[0] = oldest->desc.word[13]; raw[1] = oldest->desc.word[12];
    raw[2] = 3; /* Real MPEG4 backend FINAL/wait/unlock, no new input frame. */
    raw[3] = oldest->desc.word[0]; raw[4] = oldest->desc.word[2];
    raw[5] = 1; raw[6] = s->frame_bytes;
    memcpy(raw + 7, hal + 0x15, 13 * sizeof(*raw));
    raw[20] = 0; raw[21] = s->config.width; raw[22] = s->config.height;
    raw[23] = 16; raw[24] = 8; raw[25] = s->config.height;
    raw[26] = (uint32_t)(uintptr_t)(hal + 0x43);
    /* EncodeOneUnit writes BS parameters after the eleven API pointers in
     * the original block; never pass the smaller shadow dispatch table. */
    if (api.encode_one_unit((void *)(uintptr_t)hal[0], s->original_api, raw)) return EIO;
    int (*get_next)(void *, void *) = (void *)(uintptr_t)table[9];
    uint32_t info[10] = {0};
    if (get_next((void *)(uintptr_t)hal[0], info)) return EIO;
    uint64_t address = info[1], bytes = info[4];
    fprintf(stderr, "VENC_FINAL ts_words=%08x:%08x pending_words=%08x:%08x bytes=%u\n",
            info[9], info[8], oldest->desc.word[12], oldest->desc.word[13], info[4]);
    if (!bytes && !address) return 0;
    if (address < base || address - base > length ||
        bytes > length - (address - base) || bytes > s->bitstream.size ||
        bytes > MT6580_ENC_MAX_PACKET_BYTES || bytes < 5) return EOVERFLOW;
    memcpy((void *)(uintptr_t)s->bitstream.va, (void *)(uintptr_t)address, bytes);
    s->bs.word[8] = (uint32_t)bytes;
    s->done.word[2] = (uint32_t)(uintptr_t)&s->bs;
    uint8_t type = ((const uint8_t *)(uintptr_t)address)[4];
    s->done.word[6] = s->config.codec == 1 ? (type & 0xc0) == 0 : (~(type >> 1) & 1);
    hal[1]++; /* Preserve the wrapper's completed-frame accounting. */
    return 0;
}
static int drain(struct session *s, int emit)
{
    if (!s->opened) return EINVAL;
    unsigned limit = s->count;
    for (unsigned i = 0; i < limit && s->count; ++i) {
        prepare_output(s);
        int error = encode_final(s);
        if (error) return error;
        int produced = 0;
        error = packet(s, emit, &produced);
        if (error) return error;
        /* No fake empty EOS while a submitted frame is unaccounted for. */
        if (!produced) return EPROTO;
    }
    if (s->count) return EPROTO;
    s->draining = 1;
    return 0;
}
/* On an uncertain library/hardware stop, do not unmap any potentially live
 * input or call free on a partially destroyed instance. The caller exits this
 * worker so the native driver's fd-close stop/quarantine contract owns cleanup.
 * This recovery path itself still requires actual abort qualification. */
static int cleanup(struct session *s)
{
    /* A failing legacy initialization has no reliable partial-destroy ABI. */
    if (s->configuring) return EIO;
    if (s->instance) {
        if (s->header_attempted && !s->opened) return EIO;
        if (s->opened && s->count) {
            int error = drain(s, 0);
            if (error) return error;
        }
        if (restore_api(s)) return EIO;
        if (s->initialized && api.deinit(s->instance)) return EIO;
        if (api.release(s->instance)) return EIO;
        s->instance = NULL;
    }
    memset(s->shadow_api, 0, sizeof(s->shadow_api));
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (s->in[i].memory.va && api.free(&s->in[i].memory, sizeof(s->in[i].memory))) return EIO;
        memset(&s->in[i], 0, sizeof(s->in[i]));
    }
    if (s->bitstream.va && api.free(&s->bitstream, sizeof(s->bitstream))) return EIO;
    memset(&s->bitstream, 0, sizeof(s->bitstream));
    memset(&s->config, 0, sizeof(s->config));
    memset(s->setting, 0, sizeof(s->setting)); memset(s->extra, 0, sizeof(s->extra));
    s->frame_bytes = 0; s->head = s->count = 0;
    s->initialized = s->configuring = s->header_attempted = s->opened = s->draining = 0;
    s->open_seen = s->level_applied = 0;
    return 0;
}
static int validate_config(const struct mt6580_enc_config *c)
{
    if ((c->codec != 1 && c->codec != 4) || !c->width || !c->height ||
        c->width > MT6580_ENC_MAX_WIDTH || c->height > MT6580_ENC_MAX_HEIGHT ||
        c->width % 16 || c->height % 16 || c->bitrate < 32000 ||
        !c->frame_rate || c->frame_rate > 30 || !c->key_interval || c->key_interval > 1000)
        return EINVAL;
    if (c->codec == 1) {
        if (c->level == 0) {
            if (c->width > 176 || c->height > 144 || c->frame_rate > 15 || c->bitrate > 64000)
                return EINVAL;
        } else if (c->level == 6) {
            if (c->bitrate > 12000000) return EINVAL;
        } else return ENOTSUP;
    } else {
        if (c->level != 40 || c->bitrate > 2000000) return EINVAL;
        if (!((c->width == 128 && c->height == 96) ||
            (c->width == 176 && c->height == 144) || (c->width == 352 && c->height == 288) ||
            (c->width == 704 && c->height == 576))) return ENOTSUP;
    }
    uint64_t pixels = (uint64_t)c->width * c->height * 3 / 2;
    uint64_t aligned = (pixels + 4095) & ~UINT64_C(4095);
    return aligned * SLOTS + MT6580_ENC_MAX_PACKET_BYTES > OWNED_LIMIT ? E2BIG : 0;
}
/* Validate the header the actual codec generated; never patch a profile byte
 * to claim a level the backend did not accept. */
static int validate_sequence_level(const struct session *s, const uint8_t *data, uint32_t bytes)
{
    if (s->config.codec != 1) return 0;
    uint8_t expected = s->config.level == 6 ? 6 : 8;
    for (uint32_t i = 0; i + 5 <= bytes; ++i) {
        if (!data[i] && !data[i + 1] && data[i + 2] == 1 && data[i + 3] == 0xb0)
            return data[i + 4] == expected ? 0 : EPROTO;
    }
    return EPROTO;
}
static int configure(struct session *s, const struct mt6580_enc_config *c,
                     uint64_t token, int64_t pts)
{
    if (s->instance || s->opened) return EBUSY;
    int error = validate_config(c);
    if (error) return error;
    s->config = *c;
    s->frame_bytes = c->width * c->height * 3 / 2;
    /* Allocations precede any hardware work: the native ABI forbids buffer
     * mutations while a submitted job owns the device. */
    for (unsigned i = 0; i < SLOTS; ++i) {
        if ((error = allocate(&s->in[i].memory, s->frame_bytes))) return error;
    }
    if ((error = allocate(&s->bitstream, MT6580_ENC_MAX_PACKET_BYTES))) return error;
    s->configuring = 1;
    if (api.create(&s->instance, c->codec) || !s->instance) return EIO;
    uint32_t record[4] = {c->width, c->height, c->width, c->height};
    if (api.set(s->instance, 0x14, record, NULL)) return EIO;
    if (api.init(s->instance)) return EIO;
    s->initialized = 1;
    s->extra[0] = c->key_interval; s->extra[1] = c->bitrate;
    s->setting[0] = 3; s->setting[1] = 0x4000; s->setting[2] = 10;
    s->setting[3] = c->width; s->setting[4] = c->height;
    s->setting[5] = c->width; s->setting[6] = c->height;
    s->setting[7] = c->key_interval - 1; s->setting[8] = 0;
    s->setting[9] = c->frame_rate; s->setting[12] = (uint32_t)(uintptr_t)s->extra;
    if (api.set(s->instance, 6, s->setting, NULL)) return EIO;
    long tid = syscall(__NR_gettid);
    if (tid <= 0 || tid > UINT16_MAX) return EOVERFLOW;
    uint32_t tids = (uint32_t)tid << 16 | (uint32_t)tid;
    if (api.set(s->instance, 9, &tids, NULL)) return EIO;
    if ((error = install_open_adapter(s))) return error;
    prepare_output(s); s->header_attempted = 1;
    if (opening_session) return EBUSY;
    opening_session = s;
    int status = api.encode(s->instance, 1, NULL, &s->bs, &s->done);
    opening_session = NULL;
    if (status || !s->open_seen || !s->level_applied) return EIO;
    uint8_t *data; uint32_t bytes;
    if ((error = output_bytes(s, &data, &bytes))) return error;
    if (bytes > MT6580_ENC_MAX_CSD_BYTES || (!bytes && c->codec != 4)) return EPROTO;
    if ((error = validate_sequence_level(s, data, bytes))) return error;
    s->opened = 1; s->configuring = 0;
    return reply(s, MT6580_ENC_CSD, token, pts, 0, data, bytes);
}
static int frame(struct session *s, const uint8_t *data, size_t bytes,
                 uint64_t token, int64_t pts)
{
    if (!s->opened || s->draining || bytes != s->frame_bytes) return EINVAL;
    if (s->count == SLOTS) return EAGAIN;
    struct slot *slot = NULL;
    unsigned index = 0;
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (s->in[i].busy && s->in[i].token == token) return EALREADY;
        if (!s->in[i].busy && !slot) { slot = &s->in[i]; index = i; }
    }
    if (!slot) return EPROTO;
    memcpy((void *)(uintptr_t)slot->memory.va, data, bytes);
    if (api.flush(&slot->memory, sizeof(slot->memory), 1)) return EIO;
    memset(&slot->desc, 0, sizeof(slot->desc));
    slot->desc.word[0] = (uint32_t)slot->memory.va;
    slot->desc.word[2] = (uint32_t)slot->memory.iova;
    slot->desc.word[4] = (uint32_t)bytes;
    /* Original OMX EncodeFunc stores nTimeStamp high at descriptor+0x30
     * and low at +0x34. EncMp4Frame_hybrid reverses these into the backend's
     * ordinary little-endian raw[0]=low, raw[1]=high timestamp, in microseconds. */
    slot->desc.word[12] = (uint32_t)((uint64_t)pts >> 32);
    slot->desc.word[13] = (uint32_t)(uint64_t)pts;
    slot->token = token; slot->pts = pts; slot->busy = 1;
    s->queue[(s->head + s->count) % SLOTS] = index; s->count++;
    prepare_output(s);
    if (api.encode(s->instance, 4, &slot->desc, &s->bs, &s->done)) return EIO;
    int produced;
    return packet(s, 1, &produced);
}
static int serve(int fd)
{
    struct session *s = calloc(1, sizeof(*s));
    if (!s) return 0;
    s->fd = fd;
    const char *stage = "idle";
    unsigned requests = 0;
    int transport_error = 0;
    struct mt6580_c2_header h = {0};
    for (;;) {
        struct timeval idle = {0, 0}, active = {35, 0};
        memset(&h, 0, sizeof(h));
        stage = "idle-timeout";
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle))) {
            transport_error = errno; break;
        }
        /* Unlimited idle time, but bound a partially transmitted request. */
        stage = "first-header-byte";
        int error = transfer(fd, &h, 1, 0);
        if (error) { transport_error = error; break; }
        stage = "active-timeout";
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &active, sizeof(active))) {
            transport_error = errno; break;
        }
        stage = "remaining-header";
        if ((error = transfer(fd, (uint8_t *)&h + 1, sizeof(h) - 1, 0))) {
            transport_error = error; break;
        }
        requests++;
        stage = "validate-header";
        if (h.magic != MT6580_ENC_MAGIC || h.version != MT6580_ENC_VERSION || !h.generation ||
            h.payload_bytes > MT6580_ENC_MAX_FRAME_BYTES ||
            (h.flags & ~MT6580_ENC_FLAG_EOS) || (h.command != MT6580_ENC_DRAIN && h.flags) ||
            (s->generation && s->generation != h.generation && h.command != MT6580_ENC_RESET)) {
            transport_error = EPROTO; break;
        }
        s->generation = h.generation;
        uint8_t *payload = h.payload_bytes ? malloc(h.payload_bytes) : NULL;
        stage = "allocate-payload";
        if (h.payload_bytes && !payload) { transport_error = ENOMEM; break; }
        stage = "receive-payload";
        if (h.payload_bytes && (error = transfer(fd, payload, h.payload_bytes, 0))) {
            transport_error = error; free(payload); break;
        }
        stage = "dispatch";
        uint32_t response = MT6580_ENC_ACK;
        switch (h.command) {
        case MT6580_ENC_CONFIG:
            if (h.payload_bytes != sizeof(struct mt6580_enc_config)) error = EINVAL;
            else { struct mt6580_enc_config c; memcpy(&c, payload, sizeof(c));
                error = configure(s, &c, h.token, h.pts); }
            break;
        case MT6580_ENC_FRAME:
            error = h.payload_bytes ? frame(s, payload, h.payload_bytes, h.token, h.pts) : EINVAL;
            break;
        case MT6580_ENC_DRAIN:
            error = h.payload_bytes ? EINVAL : drain(s, 1);
            response = MT6580_ENC_DRAINED;
            break;
        case MT6580_ENC_RESET:
        case MT6580_ENC_CLOSE:
            error = h.payload_bytes ? EINVAL : cleanup(s);
            break;
        default: error = ENOTSUP;
        }
        free(payload);
        if (error) {
            int32_t status = error;
            transport_error = error;
            (void)reply(s, MT6580_ENC_ERROR, h.token, h.pts, 0, &status, sizeof(status));
            break;
        }
        stage = "reply-terminal";
        if ((error = reply(s, response, h.token, h.pts,
                  response == MT6580_ENC_DRAINED ? h.flags : 0, NULL, 0))) {
            transport_error = error; break;
        }
        if (h.command == MT6580_ENC_CLOSE) { stage = "normal-close"; break; }
    }
    fprintf(stderr, "VENC_CONNECTION stage=%s error=%d requests=%u command=%u bytes=%u "
            "generation=%u token=%llu pending=%u\n", stage, transport_error, requests,
            h.command, h.payload_bytes, h.generation, (unsigned long long)h.token, s->count);
    int error = cleanup(s);
    if (error) fprintf(stderr, "VENC_CONNECTION cleanup_error=%d\n", error);
    if (!error) free(s); /* Keep descriptors intact until fatal fd-close. */
    return error;
}
static int load_api(void)
{
    const char *directory = getenv("MT6580_CODEC_LIBDIR");
    char name[512];
    if (!directory) directory = LIBDIR;
    if (snprintf(name, sizeof(name), "%s/libvcodecdrv.so", directory) >= (int)sizeof(name)) return E2BIG;
    void *lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "mt6580 encoder: %s\n", dlerror()); return ENOENT; }
#define LOAD(field, source, symbol) do { *(void **)(&api.field) = dlsym(source, symbol); if (!api.field) return ENOENT; } while (0)
    LOAD(create, lib, "eVEncDrvCreate"); LOAD(init, lib, "eVEncDrvInit");
    LOAD(set, lib, "eVEncDrvSetParam"); LOAD(encode, lib, "eVEncDrvEncode");
    LOAD(encode_one_unit, lib, "EncodeOneUnit");
    LOAD(encode_open, lib, "EncodeOpen");
    LOAD(deinit, lib, "eVEncDrvDeInit"); LOAD(release, lib, "eVEncDrvRelease");
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
    const char *inherited = getenv("ANDROID_SOCKET_mt6580_venc");
    if (inherited) {
        char *end; long n = strtol(inherited, &end, 10);
        if (!*inherited || *end || n < 0 || n > INT32_MAX) return 2;
        server = (int)n;
        struct sockaddr_un address = {0};
        socklen_t bytes = sizeof(address), type_bytes = sizeof(int);
        int type = 0;
        if (getsockname(server, (struct sockaddr *)&address, &bytes) ||
            address.sun_family != AF_UNIX || !memchr(address.sun_path, 0, sizeof(address.sun_path)) ||
            strcmp(address.sun_path, MT6580_ENC_SOCKET) ||
            getsockopt(server, SOL_SOCKET, SO_TYPE, &type, &type_bytes) || type != SOCK_STREAM) return 2;
    } else if (argc == 3 && !strcmp(argv[1], "--socket") && getuid() == 0) {
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        if (strlen(argv[2]) >= sizeof(address.sun_path)) return 2;
        strcpy(address.sun_path, argv[2]);
        server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address))) return 1;
    } else return 2;
    if (listen(server, 1)) return 1;
    for (;;) {
        int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; return 1; }
        struct ucred cred;
        socklen_t size = sizeof(cred);
        if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) &&
            (cred.uid == 1046 || cred.uid == 0)) {
            struct timeval timeout = {35, 0};
            int error = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            if (!error) error = serve(fd);
            close(fd);
            if (error) _exit(1); /* fd-close proves stop or quarantines native DMA. */
        } else close(fd);
    }
}
