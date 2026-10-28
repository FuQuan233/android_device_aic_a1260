/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "C2Mt6580Mpeg4Enc"
#include "C2Mt6580Mpeg4Enc.h"
#include <Codec2BufferUtils.h>
#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <log/log.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace android {
namespace {
constexpr uint32_t kMagic = 0x4d544532, kVersion = 2;
constexpr uint32_t kConfig = 1, kFrame = 2, kDrain = 3;
constexpr uint32_t kAck = 0x81, kCsd = 0x82, kPacket = 0x83, kDrained = 0x84, kError = 0xff;
constexpr uint32_t kEos = 1, kKeyFrame = 2, kMaxPayload = 4 * 1024 * 1024, kMaxCsd = 65536;
constexpr char kSocket[] = "/dev/socket/mt6580-venc";
// Version 2 passes the requested level to the worker's real backend controls.
// Larger MPEG4 buffers require Level 6; a Level 0 request stays constrained
// to its own resolution/rate/bitrate instead of being silently promoted.
constexpr uint32_t maxWidth(bool h263) { return h263 ? 352u : 1280u; }
constexpr uint32_t maxHeight(bool h263) { return h263 ? 288u : 720u; }
constexpr uint32_t maxFps(bool) { return 30u; }
constexpr uint32_t maxBitrate(bool h263) { return h263 ? 2000000u : 12000000u; }
struct Header {
    uint32_t magic, version, command, bytes;
    uint64_t token;
    int64_t pts;
    uint32_t generation, flags;
};
static_assert(sizeof(Header) == 40 && offsetof(Header, token) == 16, "encoder wire ABI");
const char* componentName(bool h263) {
    return h263 ? C2Mt6580Mpeg4Enc::kH263Name : C2Mt6580Mpeg4Enc::kMpeg4Name;
}
void setCsd(const std::unique_ptr<C2Work>& work, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return;
    auto csd = C2StreamInitDataInfo::output::AllocUnique(bytes.size(), 0u);
    if (!csd) {
        work->result = C2_NO_MEMORY;
        work->worklets.front()->output.buffers.clear();
        return;
    }
    memcpy(csd->m.value, bytes.data(), bytes.size());
    work->worklets.front()->output.configUpdate.push_back(std::move(csd));
}
}

class C2Mt6580Mpeg4Enc::EncoderClient {
public:
    using Callback = std::function<bool(const Header&, const std::vector<uint8_t>&)>;
    ~EncoderClient() { disconnect(); }
    bool connected() const { return mFd.load() >= 0; }
    void disconnect() {
        std::lock_guard<std::mutex> lock(mMutex);
        int fd = mFd.exchange(-1);
        if (fd >= 0) close(fd);
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mMutex);
        int fd = mFd.load();
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
    int connect() {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mFd.load() >= 0) return 0;
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -errno;
        timeval timeout{35, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
            int error = errno; close(fd); return -error;
        }
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        static_assert(sizeof(kSocket) <= sizeof(address.sun_path), "encoder socket path");
        memcpy(address.sun_path, kSocket, sizeof(kSocket));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
            int error = errno; close(fd); return -error;
        }
        mFd.store(fd); return 0;
    }
    int transact(uint32_t command, uint64_t token, int64_t pts, uint32_t generation,
                 uint32_t flags, const void* payload, size_t bytes, const Callback& callback) {
        if (!connected() || !generation || bytes > kMaxPayload || (bytes && !payload) ||
            (flags & ~kEos) || (command != kDrain && flags)) return -EINVAL;
        Header request{kMagic, kVersion, command, static_cast<uint32_t>(bytes), token, pts, generation, flags};
        int error = transfer(&request, sizeof(request), true);
        if (!error && bytes) error = transfer(const_cast<void*>(payload), bytes, true);
        bool csdSeen = false;
        for (unsigned count = 0; !error && count < 256; ++count) {
            Header h{}; error = transfer(&h, sizeof(h), false);
            if (error) break;
            if (h.magic != kMagic || h.version != kVersion || h.generation != generation ||
                h.bytes > kMaxPayload) { error = -EPROTO; break; }
            std::vector<uint8_t> data(h.bytes);
            if (!data.empty()) error = transfer(data.data(), data.size(), false);
            if (error) break;
            if (h.command == kError) {
                int32_t status = 0;
                if (h.token == token && !h.flags && data.size() == sizeof(status))
                    memcpy(&status, data.data(), sizeof(status));
                error = status > 0 && status <= 4095 ? -status : -EPROTO; break;
            }
            if (h.command == kAck) {
                if (command == kDrain || h.token != token || h.flags || !data.empty() ||
                    (command == kConfig && !csdSeen)) { error = -EPROTO; break; }
                return 0;
            }
            if (h.command == kDrained) {
                if (command != kDrain || h.token != token || h.flags != flags || !data.empty()) {
                    error = -EPROTO; break;
                }
                return 0;
            }
            if (h.command == kCsd) {
                if (command != kConfig || h.token != token || h.flags || data.size() > kMaxCsd || csdSeen) {
                    error = -EPROTO; break;
                }
                csdSeen = true;
            } else if (h.command == kPacket) {
                if ((command != kFrame && command != kDrain) || data.empty() || (h.flags & ~kKeyFrame)) {
                    error = -EPROTO; break;
                }
            } else { error = -EPROTO; break; }
            if (!callback(h, data)) error = -ECANCELED;
        }
        disconnect(); return error ? error : -ELOOP;
    }
private:
    std::atomic<int> mFd{-1};
    mutable std::mutex mMutex;
    int transfer(void* memory, size_t bytes, bool sending) {
        uint8_t* p = static_cast<uint8_t*>(memory);
        int fd = mFd.load();
        if (fd < 0) return -EPIPE;
        while (bytes) {
            ssize_t n = sending ? send(fd, p, bytes, MSG_NOSIGNAL) : recv(fd, p, bytes, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -ETIMEDOUT : -errno;
            if (!n) return -EPIPE;
            bytes -= n; p += n;
        }
        return 0;
    }
};

class C2Mt6580Mpeg4Enc::IntfImpl : public SimpleInterface<void>::BaseParams {
public:
    IntfImpl(bool h263, const std::shared_ptr<C2ReflectorHelper>& helper)
        : SimpleInterface<void>::BaseParams(helper, componentName(h263), C2Component::KIND_ENCODER,
                C2Component::DOMAIN_VIDEO, h263 ? MEDIA_MIMETYPE_VIDEO_H263 : MEDIA_MIMETYPE_VIDEO_MPEG4),
          mH263(h263) {
        noPrivateBuffers(); noInputReferences(); noOutputReferences(); noInputLatency(); noTimeStretch();
        addParameter(DefineParam(mAttributes, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL)).build());
        addParameter(DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
            .withConstValue(new C2StreamUsageTuning::input(0u, uint64_t(C2MemoryUsage::CPU_READ))).build());
        addParameter(DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
            .withDefault(new C2StreamPixelFormatInfo::input(0u, HAL_PIXEL_FORMAT_YV12))
            .withFields({C2F(mPixelFormat, value).oneOf({HAL_PIXEL_FORMAT_YV12, HAL_PIXEL_FORMAT_YCBCR_420_888,
                HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED})})
            .withSetter(Setter<decltype(*mPixelFormat)>::StrictValueWithNoDeps).build());
        addParameter(DefineParam(mDelay, C2_PARAMKEY_OUTPUT_DELAY)
            .withConstValue(new C2PortActualDelayTuning::output(1)).build());
        if (h263) {
            addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::input(0u, 176, 144))
                .withFields({C2F(mSize, width).oneOf({176, 352}), C2F(mSize, height).oneOf({144, 288})})
                .withSetter(H263SizeSetter).build());
        } else {
            addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::input(0u, 320, 240))
                .withFields({C2F(mSize, width).inRange(16, maxWidth(false), 16), C2F(mSize, height).inRange(16, maxHeight(false), 16)})
                .withSetter(SizeSetter).build());
        }
        addParameter(DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
            .withDefault(new C2StreamFrameRateInfo::output(0u, float(maxFps(h263))))
            .withFields({C2F(mFrameRate, value).inRange(1.f, float(maxFps(h263)))})
            .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps).build());
        addParameter(DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
            .withDefault(new C2StreamBitrateInfo::output(0u, 1000000))
            .withFields({C2F(mBitrate, value).inRange(32000, maxBitrate(h263))})
            .withSetter(Setter<decltype(*mBitrate)>::StrictValueWithNoDeps).build());
        addParameter(DefineParam(mSyncInterval, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
            .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
            .withFields({C2F(mSyncInterval, value).inRange(0, 10000000)})
            .withSetter(Setter<decltype(*mSyncInterval)>::StrictValueWithNoDeps).build());
        if (h263) {
            addParameter(DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::output(0u,
                    C2Config::PROFILE_H263_BASELINE, C2Config::LEVEL_H263_40))
                .withFields({C2F(mProfileLevel, profile).equalTo(C2Config::PROFILE_H263_BASELINE),
                    C2F(mProfileLevel, level).equalTo(C2Config::LEVEL_H263_40)})
                .withSetter(ProfileSetter).build());
        } else {
            addParameter(DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::output(0u,
                    C2Config::PROFILE_MP4V_SIMPLE, C2Config::LEVEL_MP4V_6))
                .withFields({C2F(mProfileLevel, profile).equalTo(C2Config::PROFILE_MP4V_SIMPLE),
                    C2F(mProfileLevel, level).oneOf({C2Config::LEVEL_MP4V_0, C2Config::LEVEL_MP4V_6})})
                .withSetter(ProfileSetter).build());
        }
    }
    Config config_l() const {
        const uint32_t level = mH263 ? 40u :
            mProfileLevel->level == C2Config::LEVEL_MP4V_0 ? 0u :
            mProfileLevel->level == C2Config::LEVEL_MP4V_6 ? 6u : UINT32_MAX;
        uint32_t fps = std::isfinite(mFrameRate->value) && mFrameRate->value >= 1.f &&
            mFrameRate->value <= 30.f ? static_cast<uint32_t>(std::lround(mFrameRate->value)) : 0;
        // Validate the final configuration, including a fractional >15fps
        // Level 0 request, before any CONFIG reaches the native worker.
        if (!mH263 && level == 0 && mFrameRate->value > 15.f) fps = 0;
        uint32_t keyInterval = mSyncInterval->value >= 0 && mSyncInterval->value <= 10000000 ?
            static_cast<uint32_t>(std::max(1.0, std::round(double(mSyncInterval->value) * fps / 1000000.0))) : 0;
        return Config{mH263 ? 4u : 1u, mSize->width, mSize->height, mBitrate->value, fps, keyInterval, level};
    }
private:
    static C2R ProfileSetter(bool, const C2P<C2StreamProfileLevelInfo::output>& old,
            C2P<C2StreamProfileLevelInfo::output>& me) {
        // CCodec may apply a configure request in multiple C2 config calls.
        // Rejecting a supported level against the old size/rate here makes
        // a valid final batch depend on update order. Keep explicit levels
        // intact and check all cross-field limits in ensureConfigured().
        C2R r = C2R::Ok();
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            r = r.plus(C2SettingResultBuilder::BadValue(me.F(me.v.profile))); me.set().profile = old.v.profile;
        }
        if (!me.F(me.v.level).supportsAtAll(me.v.level)) {
            r = r.plus(C2SettingResultBuilder::BadValue(me.F(me.v.level))); me.set().level = old.v.level;
        }
        return r;
    }
    static C2R SizeSetter(bool, const C2P<C2StreamPictureSizeInfo::input>& old,
                         C2P<C2StreamPictureSizeInfo::input>& me) {
        C2R r = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            r = r.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width))); me.set().width = old.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            r = r.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height))); me.set().height = old.v.height;
        }
        return r;
    }
    static C2R H263SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input>& old,
                             C2P<C2StreamPictureSizeInfo::input>& me) {
        C2R r = SizeSetter(mayBlock, old, me);
        if (!((me.v.width == 176 && me.v.height == 144) || (me.v.width == 352 && me.v.height == 288))) {
            r = r.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            r = r.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().width = old.v.width; me.set().height = old.v.height;
        }
        return r;
    }
    const bool mH263;
    std::shared_ptr<C2ComponentAttributesSetting> mAttributes;
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamPixelFormatInfo::input> mPixelFormat;
    std::shared_ptr<C2PortActualDelayTuning::output> mDelay;
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncInterval;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
};

bool C2Mt6580Mpeg4Enc::Config::operator==(const Config& other) const {
    return codec == other.codec && width == other.width && height == other.height &&
        bitrate == other.bitrate && fps == other.fps && keyInterval == other.keyInterval && level == other.level;
}
c2_status_t C2Mt6580Mpeg4Enc::CreateComponent(bool h263, c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper, std::shared_ptr<C2Component>* component) {
    if (!helper || !component) return C2_BAD_VALUE;
    *component = std::make_shared<C2Mt6580Mpeg4Enc>(h263, id, std::make_shared<IntfImpl>(h263, helper));
    return C2_OK;
}
c2_status_t C2Mt6580Mpeg4Enc::CreateInterface(bool h263, c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper, std::shared_ptr<C2ComponentInterface>* intf) {
    if (!helper || !intf) return C2_BAD_VALUE;
    *intf = std::make_shared<SimpleInterface<IntfImpl>>(componentName(h263), id, std::make_shared<IntfImpl>(h263, helper));
    return C2_OK;
}
C2Mt6580Mpeg4Enc::C2Mt6580Mpeg4Enc(bool h263, c2_node_id_t id, const std::shared_ptr<IntfImpl>& intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(componentName(h263), id, intf)),
      mIntf(intf), mClient(std::make_unique<EncoderClient>()), mH263(h263) {}
C2Mt6580Mpeg4Enc::~C2Mt6580Mpeg4Enc() { onRelease(); }

void C2Mt6580Mpeg4Enc::cancelTransaction() {
    std::lock_guard<std::mutex> lock(mOutputMutex);
    mAbortEpoch.fetch_add(1); mClient->cancel();
}
c2_status_t C2Mt6580Mpeg4Enc::flush_sm(flush_mode_t mode,
        std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    if (!flushedWork || mode != FLUSH_COMPONENT) return C2_BAD_VALUE;
    cancelTransaction(); return SimpleC2Component::flush_sm(mode, flushedWork);
}
c2_status_t C2Mt6580Mpeg4Enc::stop() { cancelTransaction(); return SimpleC2Component::stop(); }
c2_status_t C2Mt6580Mpeg4Enc::reset() { cancelTransaction(); return SimpleC2Component::reset(); }
c2_status_t C2Mt6580Mpeg4Enc::release() { cancelTransaction(); return SimpleC2Component::release(); }
c2_status_t C2Mt6580Mpeg4Enc::connectWorker() {
    mClient->disconnect();
    if (mGeneration == UINT32_MAX) return C2_BAD_STATE;
    ++mGeneration;
    return mClient->connect() ? C2_CORRUPTED : C2_OK;
}
c2_status_t C2Mt6580Mpeg4Enc::onInit() {
    mPending.clear(); mDeferred.reset(); mCsd.clear(); mCsdPending = false;
    mConfigured = mError = mEos = mNeedsRestart = false;
    return connectWorker();
}
c2_status_t C2Mt6580Mpeg4Enc::onStop() {
    // EOF requests worker teardown without a blocking CLOSE transaction.
    mClient->disconnect(); mPending.clear(); mDeferred.reset(); mCsd.clear();
    mConfigured = mCsdPending = mError = mEos = false; mNeedsRestart = true;
    return C2_OK;
}
void C2Mt6580Mpeg4Enc::onReset() { onStop(); }
void C2Mt6580Mpeg4Enc::onRelease() { onStop(); }
c2_status_t C2Mt6580Mpeg4Enc::onFlush_sm() {
    onStop(); return onInit();
}

void C2Mt6580Mpeg4Enc::empty(const std::unique_ptr<C2Work>& work, bool eos) {
    if (!work) return;
    work->result = C2_OK; work->workletsProcessed = 1;
    auto& out = work->worklets.front()->output;
    out.ordinal = work->input.ordinal; out.buffers.clear();
    out.flags = eos ? C2FrameData::FLAG_END_OF_STREAM : static_cast<C2FrameData::flags_t>(0);
    if (mCsdPending) { setCsd(work, mCsd); mCsdPending = false; }
}
void C2Mt6580Mpeg4Enc::emit(Output output, const std::unique_ptr<C2Work>& current,
                           bool incomplete, bool eos) {
    std::lock_guard<std::mutex> lock(mOutputMutex);
    if (mTransactionEpoch != mAbortEpoch.load()) return;
    auto fill = [output, incomplete, eos](const std::unique_ptr<C2Work>& work) {
        work->result = C2_OK; work->workletsProcessed = 1;
        auto& out = work->worklets.front()->output;
        out.flags = static_cast<C2FrameData::flags_t>((incomplete ? C2FrameData::FLAG_INCOMPLETE : 0) |
            (eos ? C2FrameData::FLAG_END_OF_STREAM : 0));
        out.ordinal = output.ordinal; out.buffers = {output.buffer}; setCsd(work, output.csd);
    };
    if (current && output.token == current->input.ordinal.frameIndex.peeku()) {
        if (incomplete) cloneAndSend(output.token, current, fill); else fill(current);
    } else finish(output.token, fill);
    mPending.erase(output.token);
}
void C2Mt6580Mpeg4Enc::fail(const std::unique_ptr<C2Work>& current, c2_status_t status) {
    ALOGE("encoder transaction/input failed: %d", status);
    mClient->disconnect(); mError = true; mDeferred.reset();
    for (const auto& entry : mPending) {
        if (current && entry.first == current->input.ordinal.frameIndex.peeku()) continue;
        finish(entry.first, [ordinal = entry.second, status](const std::unique_ptr<C2Work>& work) {
            work->result = status; work->workletsProcessed = 1;
            work->worklets.front()->output.ordinal = ordinal;
            work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
            work->worklets.front()->output.buffers.clear();
        });
    }
    mPending.clear();
    if (current) { current->input.buffers.clear(); empty(current, false); current->result = status; }
}
c2_status_t C2Mt6580Mpeg4Enc::ensureConfigured(const std::unique_ptr<C2Work>& current,
        const std::shared_ptr<C2BlockPool>& pool) {
    Config config;
    { IntfImpl::Lock lock = mIntf->lock(); config = mIntf->config_l(); }
    if (config.codec != (mH263 ? 4u : 1u) || config.width < 16 || config.height < 16 ||
        config.width > maxWidth(mH263) || config.height > maxHeight(mH263) || (config.width % 16) || (config.height % 16) ||
        config.bitrate < 32000 || config.bitrate > maxBitrate(mH263) ||
        !config.fps || config.fps > maxFps(mH263) || !config.keyInterval || config.keyInterval > 300 ||
        (mH263 && !((config.width == 176 && config.height == 144) ||
                   (config.width == 352 && config.height == 288)))) return C2_BAD_VALUE;
    if ((mH263 && config.level != 40) ||
        (!mH263 && config.level != 0 && config.level != 6) ||
        (!mH263 && config.level == 0 && (config.width > 176 || config.height > 144 ||
             config.fps > 15 || config.bitrate > 64000))) return C2_BAD_VALUE;
    if (mConfigured) {
        // The native protocol has no in-place bitrate/size command. Do not
        // silently acknowledge new C2 settings while encoding with old ones.
        return config == mConfig ? C2_OK : C2_BAD_STATE;
    }
    static_assert(sizeof(Config) == 28 && offsetof(Config, level) == 24, "encoder CONFIG v2 wire ABI");
    mConfig = config;
    c2_status_t status = transaction(kConfig, mControlToken--, 0, 0, &config, sizeof(config), current, pool);
    if (status == C2_OK) mConfigured = true;
    return status;
}
c2_status_t C2Mt6580Mpeg4Enc::copyInput(const std::unique_ptr<C2Work>& current,
                                     std::vector<uint8_t>* i420) {
    if (current->input.buffers.size() != 1 || !current->input.buffers.front() ||
        current->input.buffers.front()->data().type() != C2BufferData::GRAPHIC ||
        current->input.buffers.front()->data().graphicBlocks().size() != 1) return C2_BAD_VALUE;
    const C2ConstGraphicBlock block = current->input.buffers.front()->data().graphicBlocks().front();
    if (block.width() < mConfig.width || block.height() < mConfig.height) return C2_BAD_VALUE;
    C2GraphicView view = block.map().get();
    if (view.error() != C2_OK) return view.error();
    if (view.crop().left || view.crop().top) return C2_BAD_VALUE;
    // Same Surface-mode crop workaround as the platform C2 MPEG4 encoder.
    view.setCrop_be(C2Rect(mConfig.width, mConfig.height));
    i420->resize(size_t(mConfig.width) * mConfig.height * 3 / 2);
    const auto& layout = view.layout();
    if (layout.type == C2PlanarLayout::TYPE_RGB || layout.type == C2PlanarLayout::TYPE_RGBA) {
        if (layout.numPlanes < 3) return C2_BAD_VALUE;
        for (unsigned i = 0; i < 3; ++i) {
            const auto& p = layout.planes[i];
            if (!view.data()[i] || p.allocatedDepth != 8 || p.bitDepth != 8 || p.rightShift ||
                p.colSampling != 1 || p.rowSampling != 1 || p.colInc <= 0 || p.rowInc <= 0 ||
                uint64_t(p.colInc) * (mConfig.width - 1) + 1 > uint32_t(p.rowInc)) return C2_BAD_VALUE;
        }
        return ConvertRGBToPlanarYUV(i420->data(), mConfig.width, mConfig.height, i420->size(), view) == OK ? C2_OK : C2_BAD_VALUE;
    }
    if (layout.type != C2PlanarLayout::TYPE_YUV || !IsYUV420(view)) return C2_BAD_VALUE;
    uint8_t* dst = i420->data();
    for (unsigned plane = 0; plane < 3; ++plane) {
        uint32_t w = plane ? mConfig.width / 2 : mConfig.width;
        uint32_t h = plane ? mConfig.height / 2 : mConfig.height;
        const auto& p = layout.planes[plane];
        if (!view.data()[plane] || p.colInc <= 0 || p.rowInc <= 0 ||
            uint64_t(p.colInc) * (w - 1) + 1 > uint32_t(p.rowInc)) return C2_BAD_VALUE;
        for (uint32_t row = 0; row < h; ++row) {
            const uint8_t* src = view.data()[plane] + size_t(row) * p.rowInc;
            if (p.colInc == 1) memcpy(dst, src, w);
            else for (uint32_t x = 0; x < w; ++x) dst[x] = src[size_t(x) * p.colInc];
            dst += w;
        }
    }
    return C2_OK;
}

c2_status_t C2Mt6580Mpeg4Enc::transaction(uint32_t command, uint64_t token, int64_t pts,
        uint32_t flags, const void* data, size_t bytes, const std::unique_ptr<C2Work>& current,
        const std::shared_ptr<C2BlockPool>& pool, bool currentEos, bool deferLast) {
    mCallbackError = C2_OK; mTransactionEpoch = mAbortEpoch.load();
    int result = mClient->transact(command, token, pts, mGeneration, flags, data, bytes,
        [&](const Header& h, const std::vector<uint8_t>& payload) {
            return response(h.command, h.token, h.pts, h.flags, payload, current, pool, currentEos, deferLast);
        });
    if (mTransactionEpoch != mAbortEpoch.load()) return C2_NOT_FOUND;
    if (!result) return C2_OK;
    return mCallbackError == C2_OK ? C2_CORRUPTED : mCallbackError;
}
bool C2Mt6580Mpeg4Enc::response(uint32_t command, uint64_t token, int64_t pts, uint32_t flags,
        const std::vector<uint8_t>& bytes, const std::unique_ptr<C2Work>& current,
        const std::shared_ptr<C2BlockPool>& pool, bool currentEos, bool deferLast) {
    if (mTransactionEpoch != mAbortEpoch.load()) return false;
    if (command == kCsd) { mCsd = bytes; mCsdPending = !bytes.empty(); return true; }
    auto pending = mPending.find(token);
    if (command != kPacket || !pool || pending == mPending.end() || pending->second.timestamp.peekll() != pts) {
        mCallbackError = C2_CORRUPTED; return false;
    }
    std::shared_ptr<C2LinearBlock> block;
    c2_status_t status = pool->fetchLinearBlock(bytes.size(),
        C2MemoryUsage{C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE}, &block);
    if (status != C2_OK || !block) { mCallbackError = status == C2_OK ? C2_NO_MEMORY : status; return false; }
    {
        C2WriteView view = block->map().get();
        if (view.error() != C2_OK || view.capacity() < bytes.size()) { mCallbackError = C2_CORRUPTED; return false; }
        memcpy(view.data(), bytes.data(), bytes.size());
    }
    auto buffer = createLinearBuffer(block, 0, bytes.size());
    if (flags & kKeyFrame) buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u, C2Config::SYNC_FRAME));
    Output out{token, pending->second, buffer, mCsdPending ? mCsd : std::vector<uint8_t>{}};
    mCsdPending = false;
    if (deferLast) {
        if (mDeferred) emit(std::move(*mDeferred), current);
        mDeferred = std::make_unique<Output>(std::move(out));
    } else emit(std::move(out), current,
        currentEos && current && token == current->input.ordinal.frameIndex.peeku());
    return true;
}
void C2Mt6580Mpeg4Enc::process(const std::unique_ptr<C2Work>& work,
        const std::shared_ptr<C2BlockPool>& pool) {
    work->result = C2_OK; work->workletsProcessed = 0;
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    work->worklets.front()->output.buffers.clear();
    if (mNeedsRestart && onInit() != C2_OK) { fail(work, C2_CORRUPTED); return; }
    if (mError || mEos || !mClient->connected()) { empty(work, false); work->result = C2_BAD_STATE; return; }
    const bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) { fail(work, C2_BAD_VALUE); return; }
    c2_status_t status = ensureConfigured(work, pool);
    if (status == C2_NOT_FOUND) {
        mPending.clear(); mDeferred.reset(); mCsdPending = false;
        work->input.buffers.clear(); empty(work, false); work->result = status; return;
    }
    if (status != C2_OK) { fail(work, status); return; }
    if (!work->input.buffers.empty()) {
        std::vector<uint8_t> i420;
        status = copyInput(work, &i420);
        if (status == C2_OK) {
            uint64_t token = work->input.ordinal.frameIndex.peeku();
            if (mPending.size() >= 64 || !mPending.emplace(token, work->input.ordinal).second) status = C2_BAD_VALUE;
            else status = transaction(kFrame, token, work->input.ordinal.timestamp.peekll(), 0,
                i420.data(), i420.size(), work, pool, eos);
        }
    } else if (!eos) empty(work, false);
    work->input.buffers.clear();
    if (status == C2_NOT_FOUND) { mPending.clear(); mDeferred.reset(); mCsdPending = false; empty(work, false); work->result = status; return; }
    if (status != C2_OK) { fail(work, status); return; }
    if (eos) {
        status = transaction(kDrain, mControlToken--, work->input.ordinal.timestamp.peekll(), kEos,
            nullptr, 0, work, pool, true);
        if (status == C2_NOT_FOUND) { mPending.clear(); mDeferred.reset(); mCsdPending = false; empty(work, false); work->result = status; return; }
        if (status != C2_OK || !mPending.empty()) { fail(work, status == C2_OK ? C2_CORRUPTED : status); return; }
        empty(work, true); mEos = true;
    }
}
c2_status_t C2Mt6580Mpeg4Enc::drain(uint32_t mode, const std::shared_ptr<C2BlockPool>& pool) {
    if (mode == NO_DRAIN) return C2_OK;
    if (mode == DRAIN_CHAIN) return C2_OMITTED;
    const bool eos = mode == DRAIN_COMPONENT_WITH_EOS;
    if (mError || !mClient->connected()) return C2_BAD_STATE;
    if (!mConfigured) { mEos = eos; return C2_OK; }
    const std::unique_ptr<C2Work> noWork;
    c2_status_t status = transaction(kDrain, mControlToken--, 0, eos ? kEos : 0,
        nullptr, 0, noWork, pool, false, true);
    if (status == C2_NOT_FOUND) { mPending.clear(); mDeferred.reset(); return status; }
    size_t expected = mDeferred ? 1 : 0;
    if (status != C2_OK || mPending.size() != expected) {
        fail(noWork, status == C2_OK ? C2_CORRUPTED : status); return status == C2_OK ? C2_CORRUPTED : status;
    }
    if (mDeferred) { emit(std::move(*mDeferred), noWork, false, eos); mDeferred.reset(); }
    if (eos) { mEos = true; return C2_OK; }
    // The old encoder's drain finishes that stream; reconnect to initialize
    // a fresh one for subsequent input instead of reusing flushed state.
    onStop(); return onInit();
}
}
