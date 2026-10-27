/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A1260_MT6580_C2_WORKER_PROTOCOL_H
#define A1260_MT6580_C2_WORKER_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

/* Local ARM little-endian wire protocol; no pointers or C++ objects cross it. */
#define MT6580_C2_SOCKET "/dev/socket/mt6580-vcodec"
#define MT6580_C2_MAGIC UINT32_C(0x4d544332)
#define MT6580_C2_VERSION UINT32_C(1)
#define MT6580_C2_MAX_WIDTH UINT32_C(1280)
#define MT6580_C2_MAX_HEIGHT UINT32_C(720)
#define MT6580_C2_MAX_AU_BYTES UINT32_C(4194304)
#define MT6580_C2_MAX_CSD_BYTES UINT32_C(65536)
#define MT6580_C2_MAX_FRAME_BYTES (MT6580_C2_MAX_WIDTH * MT6580_C2_MAX_HEIGHT * 3 / 2)
#define MT6580_C2_FLAG_EOS UINT32_C(1)

enum mt6580_c2_command {
    MT6580_C2_CONFIG = 1,
    MT6580_C2_AU = 2,
    MT6580_C2_DRAIN = 3,
    MT6580_C2_RESET = 4,
    MT6580_C2_CLOSE = 5,
    MT6580_C2_ACK = 0x81,
    MT6580_C2_FORMAT = 0x82,
    MT6580_C2_FRAME = 0x83,
    MT6580_C2_DRAINED = 0x84,
    MT6580_C2_ERROR = 0xff,
};

struct mt6580_c2_header {
    uint32_t magic;
    uint32_t version;
    uint32_t command;
    uint32_t payload_bytes;
    uint64_t token;
    int64_t pts;
    uint32_t generation;
    uint32_t flags;
};
struct mt6580_c2_format { uint32_t width, height; };
struct mt6580_c2_frame_info { uint32_t width, height, bytes, reserved; };

/* CONFIG accepts Annex-B CSD fragments and ends in ACK. AU emits zero or
 * more FRAME responses followed by ACK for that AU, without requiring a
 * current frame to be displayed. FRAME carries its originating AU token/PTS.
 * DRAIN emits remaining FRAMEs followed by DRAINED (no additional ACK).
 * DRAIN request FLAG_EOS is mirrored only by its final DRAINED response;
 * all FRAME, FORMAT, ACK and ERROR response flags are zero. RESET/CLOSE end
 * in ACK. RESET discards old-generation state; CONFIG is resent afterward.
 * FORMAT is 8 bytes; FRAME is frame_info + exactly width*height*3/2 packed
 * I420 bytes; ERROR is one signed int32 positive errno. reserved must be zero.
 * Every response preserves the request/session generation. Control tokens
 * are opaque and are not frame identities. Stream transport must use full
 * reads/writes: a frame is larger than a typical Unix packet send buffer.
 */
#if defined(__cplusplus)
static_assert(sizeof(mt6580_c2_header) == 40, "wire header must be 40 bytes");
static_assert(offsetof(mt6580_c2_header, token) == 16, "wire token offset");
static_assert(sizeof(mt6580_c2_frame_info) == 16, "wire frame header");
#else
_Static_assert(sizeof(struct mt6580_c2_header) == 40, "wire header must be 40 bytes");
_Static_assert(offsetof(struct mt6580_c2_header, token) == 16, "wire token offset");
_Static_assert(sizeof(struct mt6580_c2_frame_info) == 16, "wire frame header");
#endif
#endif
