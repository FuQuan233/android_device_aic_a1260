/* SPDX-License-Identifier: Apache-2.0 */
#include "LegacyClient.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace android::mt6580 {
LegacyClient::~LegacyClient() { disconnect(); }
void LegacyClient::disconnect() {
    std::lock_guard<std::mutex> lock(mFdMutex);
    if (mFd >= 0) close(mFd);
    mFd = -1;
}
bool LegacyClient::connected() const {
    std::lock_guard<std::mutex> lock(mFdMutex);
    return mFd >= 0;
}
void LegacyClient::cancel() {
    std::lock_guard<std::mutex> lock(mFdMutex);
    if (mFd >= 0) shutdown(mFd, SHUT_RDWR);
}
int LegacyClient::connect() {
    std::lock_guard<std::mutex> lock(mFdMutex);
    if (mFd >= 0) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    timeval timeout = {35, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        int error = errno; close(fd); return -error;
    }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    static_assert(sizeof(MT6580_C2_SOCKET) <= sizeof(address.sun_path), "socket path");
    memcpy(address.sun_path, MT6580_C2_SOCKET, sizeof(MT6580_C2_SOCKET));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        int error = errno; close(fd); return -error;
    }
    mFd = fd;
    return 0;
}
int LegacyClient::transfer(void* data, size_t bytes, bool sending) {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (bytes) {
        ssize_t n = sending ? send(mFd, p, bytes, MSG_NOSIGNAL) : recv(mFd, p, bytes, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -ETIMEDOUT : -errno;
        if (!n) return -EPIPE;
        bytes -= n; p += n;
    }
    return 0;
}
int LegacyClient::transact(uint32_t command, uint64_t token, int64_t pts,
        uint32_t generation, uint32_t flags, const void* payload, size_t bytes,
        const Callback& callback) {
    if (mFd < 0 || bytes > MT6580_C2_MAX_AU_BYTES || (bytes && !payload) || !generation)
        return -EINVAL;
    const bool drain = command == MT6580_C2_DRAIN;
    if (flags & ~MT6580_C2_FLAG_EOS || (!drain && flags)) return -EINVAL;
    mt6580_c2_header request = {MT6580_C2_MAGIC, MT6580_C2_VERSION, command,
        static_cast<uint32_t>(bytes), token, pts, generation, flags};
    int error = transfer(&request, sizeof(request), true);
    if (!error && bytes) error = transfer(const_cast<void*>(payload), bytes, true);
    /* Bound a transaction even when the peer keeps sending valid empty frames. */
    for (unsigned int packets = 0; !error && packets < 256; ++packets) {
        mt6580_c2_header response = {};
        error = transfer(&response, sizeof(response), false);
        if (error) break;
        if (response.magic != MT6580_C2_MAGIC || response.version != MT6580_C2_VERSION ||
            response.payload_bytes > MT6580_C2_MAX_FRAME_BYTES + sizeof(mt6580_c2_frame_info) ||
            response.generation > generation || !response.generation) {
            error = -EPROTO; break;
        }
        std::vector<uint8_t> data(response.payload_bytes);
        if (!data.empty()) error = transfer(data.data(), data.size(), false);
        if (error) break;
        if (response.generation != generation) continue; // reset invalidates old output
        switch (response.command) {
        case MT6580_C2_FORMAT: {
            if (response.flags || data.size() != sizeof(mt6580_c2_format)) {
                error = -EPROTO; break;
            }
            mt6580_c2_format format;
            memcpy(&format, data.data(), sizeof(format));
            if (!format.width || !format.height || (format.width & 1) || (format.height & 1) ||
                format.width > MT6580_C2_MAX_WIDTH || format.height > MT6580_C2_MAX_HEIGHT)
                error = -EPROTO;
            break;
        }
        case MT6580_C2_FRAME: {
            if (response.flags || data.size() < sizeof(mt6580_c2_frame_info)) {
                error = -EPROTO; break;
            }
            mt6580_c2_frame_info frame;
            memcpy(&frame, data.data(), sizeof(frame));
            if (!frame.width || !frame.height || (frame.width & 1) || (frame.height & 1) ||
                frame.width > MT6580_C2_MAX_WIDTH || frame.height > MT6580_C2_MAX_HEIGHT ||
                frame.reserved || frame.bytes != frame.width * frame.height * 3 / 2 ||
                data.size() != sizeof(frame) + frame.bytes) error = -EPROTO;
            break;
        }
        case MT6580_C2_ERROR: {
            int32_t status = 0;
            if (!response.flags && data.size() == sizeof(status))
                memcpy(&status, data.data(), sizeof(status));
            error = status > 0 && status <= 4095 ? -status : -EPROTO;
            break;
        }
        case MT6580_C2_ACK:
            if (drain || response.token != token || response.flags || !data.empty()) {
                error = -EPROTO; break;
            }
            return 0;
        case MT6580_C2_DRAINED:
            if (!drain || response.token != token || response.flags != flags || !data.empty()) {
                error = -EPROTO; break;
            }
            return 0;
        default:
            error = -EPROTO;
        }
        if (!error && callback && !callback(response, data)) error = -ECANCELED;
    }
    disconnect();
    return error ? error : -ELOOP;
}
}
