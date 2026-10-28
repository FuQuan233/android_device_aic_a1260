/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A1260_MT6580_ENCODER_PROTOCOL_H
#define A1260_MT6580_ENCODER_PROTOCOL_H
#include "worker_protocol.h"

/* Same pointer-free, 40-byte little-endian mt6580_c2_header as the decoder.
 * A different socket/magic prevents accidental cross-protocol dispatch. */
#define MT6580_ENC_SOCKET "/dev/socket/mt6580-venc"
#define MT6580_ENC_MAGIC UINT32_C(0x4d544532)
#define MT6580_ENC_VERSION UINT32_C(2)
#define MT6580_ENC_MAX_WIDTH UINT32_C(1280)
#define MT6580_ENC_MAX_HEIGHT UINT32_C(720)
#define MT6580_ENC_MAX_FRAME_BYTES (MT6580_ENC_MAX_WIDTH * MT6580_ENC_MAX_HEIGHT * 3 / 2)
#define MT6580_ENC_MAX_PACKET_BYTES UINT32_C(4194304)
#define MT6580_ENC_MAX_CSD_BYTES UINT32_C(65536)
#define MT6580_ENC_FLAG_EOS UINT32_C(1)
#define MT6580_ENC_FLAG_KEY_FRAME UINT32_C(2)

enum mt6580_enc_command {
    MT6580_ENC_CONFIG = 1,
    MT6580_ENC_FRAME = 2,
    MT6580_ENC_DRAIN = 3,
    MT6580_ENC_RESET = 4,
    MT6580_ENC_CLOSE = 5,
    MT6580_ENC_ACK = 0x81,
    MT6580_ENC_CSD = 0x82,
    MT6580_ENC_PACKET = 0x83,
    MT6580_ENC_DRAINED = 0x84,
    MT6580_ENC_ERROR = 0xff,
};
struct mt6580_enc_config {
    uint32_t codec; /* Stock format 1 = MPEG4, 4 = H263; never AVC. */
    uint32_t width, height;
    uint32_t bitrate; /* bits/second */
    uint32_t frame_rate; /* integer frames/second */
    uint32_t key_interval; /* frames; B frames are disabled */
    uint32_t level; /* MPEG4 Simple: 0 or 6; H263 Baseline: 40. */
};
/* Protocol v2 adds the explicit level field; v1 requests are rejected.
 * MPEG4 Level 0 is limited to 176x144/15fps/64kbit/s; Level 6 to
 * 1280x720/30fps/12Mbit/s. Levels are applied through the real backend API
 * before its first generated sequence header, which is checked as returned.
 * CONFIG takes exactly config, emits CSD (raw sequence header, possibly empty
 * for H263), then ACK with the CONFIG token/PTS. FRAME takes exactly packed
 * I420 pixels, emits zero or more PACKETs and ACK for that input. PACKET token
 * and PTS identify its originating input, including a previous pipeline input;
 * its only possible flag is KEY_FRAME. CSD/ACK/ERROR flags are zero.
 * DRAIN takes no payload, may carry EOS, emits remaining PACKETs and DRAINED
 * carrying the same EOS flag, with no additional ACK. RESET/CLOSE take no
 * payload and finish actual stream cleanup before ACK. RESET discards pending
 * old-generation output and is the only request allowed to change generation.
 * ERROR payload is one signed int32 positive errno. No pointers cross IPC.
 * Unsupported formats, alignment, stream changes and exhausted pending queues
 * fail explicitly; profile/codec feature support is not implied by the wire.
 */
#if defined(__cplusplus)
static_assert(sizeof(mt6580_enc_config) == 28, "encoder config ABI");
#else
_Static_assert(sizeof(struct mt6580_enc_config) == 28, "encoder config ABI");
#endif
#endif
