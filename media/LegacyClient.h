/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "worker_protocol.h"
#include <functional>
#include <mutex>
#include <vector>

namespace android::mt6580 {
class LegacyClient final {
public:
    using Callback = std::function<bool(const mt6580_c2_header&, const std::vector<uint8_t>&)>;
    LegacyClient() = default;
    ~LegacyClient();
    LegacyClient(const LegacyClient&) = delete;
    LegacyClient& operator=(const LegacyClient&) = delete;
    int connect();
    void disconnect();
    bool connected() const;
    // Binder flush/stop may interrupt the looper without closing/reusing its fd.
    void cancel();
    int transact(uint32_t command, uint64_t token, int64_t pts,
                 uint32_t generation, uint32_t flags,
                 const void* payload, size_t bytes, const Callback& callback);
private:
    mutable std::mutex mFdMutex;
    int mFd = -1;
    int transfer(void* data, size_t bytes, bool sending);
};
}
