// SPDX-License-Identifier: Apache-2.0
// MT6580's fixed Utgard wire ABI carries metadata before the image DMA-BUF.
#include "bufferinfo/BufferInfoMapperMetadata.h"
#include "bufferinfo/BufferInfo.h"

#include <cstring>
#include <utils/Errors.h>

namespace android::drm_hwcomposer {
int BufferInfoMapperMetadata::GetFds(buffer_handle_t handle, BufferInfo* bo) {
    if (handle == nullptr || bo == nullptr || handle->version != sizeof(native_handle_t) ||
        handle->numFds != 2 || handle->numInts != 37) {
        return BAD_VALUE;
    }
    uint32_t magic = 0;
    uint32_t flags = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(handle);
    std::memcpy(&magic, bytes + 0x50, sizeof(magic));
    std::memcpy(&flags, bytes + 0x54, sizeof(flags));
    if (magic != 0x03141592 || flags != 4 || handle->data[1] < 0 || bo->sizes[0] <= 0) {
        return BAD_VALUE;
    }
    for (int i = 1; i < kBufferMaxPlanes; ++i) {
        if (bo->sizes[i] != 0) {
            return BAD_VALUE;
        }
    }
    bo->prime_fds[0] = handle->data[1];
    return OK;
}
} // namespace android::drm_hwcomposer
