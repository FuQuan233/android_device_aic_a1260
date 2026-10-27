/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "C2Mt6580AvcDec"
#include "C2Mt6580AvcDec.h"
#include <SimpleC2Interface.h>
#include <C2PlatformSupport.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <system/graphics.h>
#include <log/log.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace android {
class C2Mt6580AvcDec::IntfImpl : public SimpleInterface<void>::BaseParams {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper>& helper)
        : SimpleInterface<void>::BaseParams(helper, kName, C2Component::KIND_DECODER,
                C2Component::DOMAIN_VIDEO, MEDIA_MIMETYPE_VIDEO_AVC) {
        // Worker owns its native DPB. It never retains a C2 input/graphic block.
        noPrivateBuffers();
        noInputReferences(); noOutputReferences(); noInputLatency(); noTimeStretch();
        addParameter(DefineParam(mAttributes, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL)).build());
        addParameter(DefineParam(mDelay, C2_PARAMKEY_OUTPUT_DELAY)
            .withConstValue(new C2PortActualDelayTuning::output(16)).build());
        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
            .withFields({C2F(mSize, width).inRange(16, MT6580_C2_MAX_WIDTH, 2),
                         C2F(mSize, height).inRange(16, MT6580_C2_MAX_HEIGHT, 2)})
            .withSetter(SizeSetter).build());
        addParameter(DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
            .withDefault(new C2StreamMaxPictureSizeTuning::output(
                0u, MT6580_C2_MAX_WIDTH, MT6580_C2_MAX_HEIGHT))
            .withFields({C2F(mMaxSize, width).inRange(16, MT6580_C2_MAX_WIDTH, 2),
                         C2F(mMaxSize, height).inRange(16, MT6580_C2_MAX_HEIGHT, 2)})
            .withSetter(MaxSizeSetter, mSize).build());
        addParameter(DefineParam(mProfile, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(0u,
                C2Config::PROFILE_AVC_BASELINE, C2Config::LEVEL_AVC_4))
            .withFields({C2F(mProfile, profile).oneOf({C2Config::PROFILE_AVC_BASELINE,
                             C2Config::PROFILE_AVC_CONSTRAINED_BASELINE}),
                         C2F(mProfile, level).oneOf({C2Config::LEVEL_AVC_1,
                             C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
                             C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
                             C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1,
                             C2Config::LEVEL_AVC_2_2, C2Config::LEVEL_AVC_3,
                             C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_4})})
            .withSetter(ProfileSetter).build());
        addParameter(DefineParam(mMaxInput, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
            .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, MT6580_C2_MAX_AU_BYTES)).build());
        addParameter(DefineParam(mPixel, C2_PARAMKEY_PIXEL_FORMAT)
            .withConstValue(new C2StreamPixelFormatInfo::output(0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
            .build());
        helper->addStructDescriptors<C2ChromaOffsetStruct>();
        addParameter(DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
            .withConstValue(C2StreamColorInfo::output::AllocShared(
                {C2ChromaOffsetStruct::ITU_YUV_420_0()}, 0u, 8u, C2Color::YUV_420)).build());
        addParameter(DefineParam(mColor, C2_PARAMKEY_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::output(0u,
                C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({C2F(mColor, range).inRange(C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
                C2F(mColor, primaries).inRange(C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                C2F(mColor, transfer).inRange(C2Color::TRANSFER_UNSPECIFIED, C2Color::TRANSFER_OTHER),
                C2F(mColor, matrix).inRange(C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)})
            .withSetter(ColorSetter).build());
    }
    std::shared_ptr<C2StreamColorAspectsInfo::output> color_l() const { return mColor; }
private:
    static C2R SizeSetter(bool, const C2P<C2StreamPictureSizeInfo::output>& old,
                         C2P<C2StreamPictureSizeInfo::output>& me) {
        C2R result = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            result = result.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = old.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            result = result.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = old.v.height;
        }
        return result;
    }
    static C2R MaxSizeSetter(bool, C2P<C2StreamMaxPictureSizeTuning::output>& me,
                            const C2P<C2StreamPictureSizeInfo::output>& size) {
        me.set().width = std::min(MT6580_C2_MAX_WIDTH, std::max(me.v.width, size.v.width));
        me.set().height = std::min(MT6580_C2_MAX_HEIGHT, std::max(me.v.height, size.v.height));
        return me.F(me.v.width).validatePossible(me.v.width).plus(
            me.F(me.v.height).validatePossible(me.v.height));
    }
    static C2R ProfileSetter(bool, const C2P<C2StreamProfileLevelInfo::input>& old,
                            C2P<C2StreamProfileLevelInfo::input>& me) {
        C2R result = C2R::Ok();
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            result = result.plus(C2SettingResultBuilder::BadValue(me.F(me.v.profile)));
            me.set().profile = old.v.profile;
        }
        if (!me.F(me.v.level).supportsAtAll(me.v.level)) {
            result = result.plus(C2SettingResultBuilder::BadValue(me.F(me.v.level)));
            me.set().level = old.v.level;
        }
        return result;
    }
    static C2R ColorSetter(bool, C2P<C2StreamColorAspectsInfo::output>& me) {
        me.set().range = std::min(me.v.range, C2Color::RANGE_OTHER);
        me.set().primaries = std::min(me.v.primaries, C2Color::PRIMARIES_OTHER);
        me.set().transfer = std::min(me.v.transfer, C2Color::TRANSFER_OTHER);
        me.set().matrix = std::min(me.v.matrix, C2Color::MATRIX_OTHER);
        return C2R::Ok();
    }
    std::shared_ptr<C2ComponentAttributesSetting> mAttributes;
    std::shared_ptr<C2PortActualDelayTuning::output> mDelay;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfile;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInput;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixel;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColor;
};

c2_status_t C2Mt6580AvcDec::CreateComponent(c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper, std::shared_ptr<C2Component>* component) {
    if (!component || !helper) return C2_BAD_VALUE;
    *component = std::make_shared<C2Mt6580AvcDec>(id, std::make_shared<IntfImpl>(helper));
    return C2_OK;
}
c2_status_t C2Mt6580AvcDec::CreateInterface(c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper,
        std::shared_ptr<C2ComponentInterface>* interface) {
    if (!interface || !helper) return C2_BAD_VALUE;
    *interface = std::make_shared<SimpleInterface<IntfImpl>>(kName, id, std::make_shared<IntfImpl>(helper));
    return C2_OK;
}
C2Mt6580AvcDec::C2Mt6580AvcDec(c2_node_id_t id, const std::shared_ptr<IntfImpl>& interface)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(kName, id, interface)), mIntf(interface) {}
C2Mt6580AvcDec::~C2Mt6580AvcDec() { onRelease(); }

void C2Mt6580AvcDec::cancelTransaction() {
    mAbortEpoch.fetch_add(1);
    mClient.cancel();
}
c2_status_t C2Mt6580AvcDec::flush_sm(flush_mode_t mode,
        std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    if (!flushedWork || mode != FLUSH_COMPONENT) return C2_BAD_VALUE;
    std::lock_guard<std::mutex> lock(mOutputMutex);
    cancelTransaction();
    return SimpleC2Component::flush_sm(mode, flushedWork);
}
c2_status_t C2Mt6580AvcDec::stop() {
    cancelTransaction(); return SimpleC2Component::stop();
}
c2_status_t C2Mt6580AvcDec::reset() {
    cancelTransaction(); return SimpleC2Component::reset();
}
c2_status_t C2Mt6580AvcDec::release() {
    cancelTransaction(); return SimpleC2Component::release();
}

void C2Mt6580AvcDec::empty(const std::unique_ptr<C2Work>& work, bool eos) {
    if (!work) return;
    work->result = C2_OK; work->workletsProcessed = 1;
    auto& out = work->worklets.front()->output;
    out.flags = eos ? C2FrameData::FLAG_END_OF_STREAM : static_cast<C2FrameData::flags_t>(0);
    out.ordinal = work->input.ordinal;
    out.buffers.clear();
}
void C2Mt6580AvcDec::emit(Output output, const std::unique_ptr<C2Work>& current, bool incomplete, bool eos) {
    std::lock_guard<std::mutex> lock(mOutputMutex);
    if (mTransactionEpoch != mAbortEpoch.load()) return;
    auto fill = [output, incomplete, eos](const std::unique_ptr<C2Work>& work) {
        work->result = C2_OK; work->workletsProcessed = 1;
        auto& out = work->worklets.front()->output;
        out.flags = static_cast<C2FrameData::flags_t>((incomplete ? C2FrameData::FLAG_INCOMPLETE : 0) |
                (eos ? C2FrameData::FLAG_END_OF_STREAM : 0));
        out.ordinal = output.ordinal;
        out.buffers = {output.buffer};
        if (output.config) out.configUpdate.push_back(
            std::make_unique<C2StreamPictureSizeInfo::output>(0u, output.width, output.height));
    };
    if (current && output.token == current->input.ordinal.frameIndex.peeku()) {
        if (incomplete) cloneAndSend(output.token, current, fill);
        else fill(current);
    } else finish(output.token, fill);
    mPending.erase(output.token);
}
void C2Mt6580AvcDec::finishEmptyPending(const std::unique_ptr<C2Work>& current,
        c2_status_t error, uint64_t except) {
    for (auto it = mPending.begin(); it != mPending.end();) {
        uint64_t token = it->first;
        C2WorkOrdinalStruct ordinal = it->second;
        if (token == except) { ++it; continue; }
        it = mPending.erase(it);
        if (current && token == current->input.ordinal.frameIndex.peeku()) continue;
        finish(token, [ordinal, error](const std::unique_ptr<C2Work>& work) {
            work->result = error; work->workletsProcessed = 1;
            work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
            work->worklets.front()->output.ordinal = ordinal;
            work->worklets.front()->output.buffers.clear();
        });
    }
}
void C2Mt6580AvcDec::fail(const std::unique_ptr<C2Work>& current, c2_status_t error) {
    ALOGE("legacy decode transaction failed: %d", error);
    mError = true; mClient.disconnect();
    mDeferred.reset();
    finishEmptyPending(current, error);
    if (current) { empty(current, false); current->result = error; }
}

c2_status_t C2Mt6580AvcDec::transaction(uint32_t command, uint64_t token, int64_t pts,
        uint32_t flags, const void* bytes, size_t size, const std::unique_ptr<C2Work>& current,
        const std::shared_ptr<C2BlockPool>& pool, bool currentEos, bool deferLast) {
    mCallbackError = C2_OK;
    mTransactionEpoch = mAbortEpoch.load();
    int result = mClient.transact(command, token, pts, mGeneration, flags, bytes, size,
        [&](const mt6580_c2_header& header, const std::vector<uint8_t>& data) {
            return response(header, data, current, pool, currentEos, deferLast);
        });
    if (mTransactionEpoch != mAbortEpoch.load()) return C2_NOT_FOUND;
    if (!result) return C2_OK;
    return mCallbackError != C2_OK ? mCallbackError : C2_CORRUPTED;
}
bool C2Mt6580AvcDec::response(const mt6580_c2_header& header, const std::vector<uint8_t>& bytes,
        const std::unique_ptr<C2Work>& current, const std::shared_ptr<C2BlockPool>& pool,
        bool currentEos, bool deferLast) {
    if (mTransactionEpoch != mAbortEpoch.load()) return false;
    if (header.command == MT6580_C2_FORMAT) {
        mt6580_c2_format format;
        memcpy(&format, bytes.data(), sizeof(format));
        if (format.width < 16 || format.height < 16 ||
            (mHaveFormat && (format.width != mWidth || format.height != mHeight))) {
            // Adaptive playback is deliberately not advertised by this component.
            mCallbackError = C2_BAD_VALUE; return false;
        }
        C2StreamPictureSizeInfo::output size(0u, format.width, format.height);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        c2_status_t status = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
        if (status != C2_OK || !failures.empty()) { mCallbackError = C2_BAD_VALUE; return false; }
        mFormatPending = true; mHaveFormat = true;
        mWidth = format.width; mHeight = format.height;
        return true;
    }
    if (header.command != MT6580_C2_FRAME || !pool || !mHaveFormat) {
        mCallbackError = C2_CORRUPTED; return false;
    }
    auto pending = mPending.find(header.token);
    if (pending == mPending.end() || pending->second.timestamp.peekll() != header.pts) {
        mCallbackError = C2_CORRUPTED; return false;
    }
    mt6580_c2_frame_info frame;
    memcpy(&frame, bytes.data(), sizeof(frame));
    if (frame.width != mWidth || frame.height != mHeight) {
        mCallbackError = C2_CORRUPTED; return false;
    }
    std::shared_ptr<C2GraphicBlock> block;
    c2_status_t status = pool->fetchGraphicBlock((frame.width + 15) & ~15u,
        (frame.height + 15) & ~15u, HAL_PIXEL_FORMAT_YV12,
        C2MemoryUsage{C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE}, &block);
    if (status != C2_OK || !block) { mCallbackError = status == C2_OK ? C2_NO_MEMORY : status; return false; }
    {
        C2GraphicView view = block->map().get();
        if (view.error() != C2_OK || view.layout().type != C2PlanarLayout::TYPE_YUV ||
            view.layout().numPlanes != 3) { mCallbackError = C2_CORRUPTED; return false; }
        const uint8_t* source = bytes.data() + sizeof(frame);
        for (unsigned int plane = 0; plane < 3; ++plane) {
            uint32_t width = plane ? frame.width / 2 : frame.width;
            uint32_t height = plane ? frame.height / 2 : frame.height;
            const C2PlaneInfo& layout = view.layout().planes[plane];
            uint8_t* destination = view.data()[plane];
            if (!destination || layout.allocatedDepth != 8 || layout.bitDepth != 8 ||
                layout.rightShift || layout.colInc <= 0 || layout.rowInc <= 0 ||
                layout.colSampling != (plane ? 2u : 1u) ||
                layout.rowSampling != (plane ? 2u : 1u) ||
                static_cast<uint64_t>(layout.colInc) * (width - 1) + 1 >
                    static_cast<uint32_t>(layout.rowInc)) {
                mCallbackError = C2_CORRUPTED; return false;
            }
            for (uint32_t row = 0; row < height; ++row) {
                uint8_t* to = destination + static_cast<size_t>(row) * layout.rowInc;
                if (layout.colInc == 1) memcpy(to, source + static_cast<size_t>(row) * width, width);
                else for (uint32_t x = 0; x < width; ++x)
                    to[static_cast<size_t>(x) * layout.colInc] = source[static_cast<size_t>(row) * width + x];
            }
            source += static_cast<size_t>(width) * height;
        }
    } // unmap graphic memory before giving the block to the consumer
    auto buffer = createGraphicBuffer(block, C2Rect(frame.width, frame.height));
    {
        IntfImpl::Lock lock = mIntf->lock();
        buffer->setInfo(mIntf->color_l());
    }
    if (mTransactionEpoch != mAbortEpoch.load()) return false;
    Output output{header.token, pending->second, buffer, frame.width, frame.height, mFormatPending};
    mFormatPending = false;
    if (deferLast) {
        if (mDeferred) emit(*mDeferred, current);
        mDeferred = std::make_unique<Output>(std::move(output));
    } else emit(std::move(output), current,
        currentEos && current && header.token == current->input.ordinal.frameIndex.peeku());
    return true;
}

c2_status_t C2Mt6580AvcDec::onInit() {
    mNeedsRestart = false;
    mError = false; mEos = false; mHaveFormat = false; mFormatPending = false;
    mPending.clear(); mDeferred.reset(); mWidth = mHeight = 0;
    if (mClient.connect()) return C2_CORRUPTED;
    if (!mCsd.empty()) {
        const std::unique_ptr<C2Work> noWork;
        return transaction(MT6580_C2_CONFIG, mControlToken--, 0, 0,
            mCsd.data(), mCsd.size(), noWork, nullptr);
    }
    return C2_OK;
}
c2_status_t C2Mt6580AvcDec::resetWorker(bool replayCsd) {
    if (mGeneration == UINT32_MAX) { mClient.disconnect(); return C2_BAD_STATE; }
    ++mGeneration;
    mPending.clear(); mDeferred.reset(); mHaveFormat = false; mFormatPending = false;
    mError = mEos = false; mWidth = mHeight = 0;
    if (!mClient.connected() && mClient.connect()) return C2_CORRUPTED;
    const std::unique_ptr<C2Work> noWork;
    c2_status_t status = transaction(MT6580_C2_RESET, mControlToken--, 0, 0,
        nullptr, 0, noWork, nullptr);
    if (status == C2_OK && replayCsd && !mCsd.empty())
        status = transaction(MT6580_C2_CONFIG, mControlToken--, 0, 0,
            mCsd.data(), mCsd.size(), noWork, nullptr);
    return status;
}
c2_status_t C2Mt6580AvcDec::onFlush_sm() {
    // flush_sm shut down any in-flight stream. A new connection cannot contain
    // stale packets even when the worker was inside the old decoder call.
    mClient.disconnect();
    c2_status_t status = resetWorker(true);
    if (status != C2_OK) mError = true;
    return status;
}
c2_status_t C2Mt6580AvcDec::onStop() {
    // EOF causes backend cleanup; never wait 35s for CLOSE during stop.
    mClient.disconnect(); mPending.clear(); mDeferred.reset();
    mHaveFormat = mFormatPending = mError = mEos = false;
    mNeedsRestart = true;
    return C2_OK;
}
void C2Mt6580AvcDec::onReset() { onStop(); mCsd.clear(); }
void C2Mt6580AvcDec::onRelease() { onStop(); mCsd.clear(); }

void C2Mt6580AvcDec::process(const std::unique_ptr<C2Work>& work,
        const std::shared_ptr<C2BlockPool>& pool) {
    work->result = C2_OK; work->workletsProcessed = 0;
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    work->worklets.front()->output.buffers.clear();
    if (mNeedsRestart && onInit() != C2_OK) { fail(work, C2_CORRUPTED); return; }
    if (mError || mEos || !mClient.connected()) { empty(work, false); work->result = C2_BAD_STATE; return; }
    const bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    const bool config = (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) != 0;
    std::vector<uint8_t> data;
    for (const auto& buffer : work->input.buffers) {
        if (!buffer || buffer->data().type() != C2BufferData::LINEAR) { fail(work, C2_BAD_VALUE); return; }
        for (const C2ConstLinearBlock& block : buffer->data().linearBlocks()) {
            C2ReadView view = block.map().get();
            if (view.error() != C2_OK || view.capacity() > MT6580_C2_MAX_AU_BYTES - data.size()) {
                fail(work, C2_BAD_VALUE); return;
            }
            data.insert(data.end(), view.data(), view.data() + view.capacity());
        }
    }
    uint64_t token = work->input.ordinal.frameIndex.peeku();
    int64_t pts = work->input.ordinal.timestamp.peekll();
    c2_status_t status = C2_OK;
    if (config && !data.empty()) {
        // MediaCodec AVC CSD is Annex-B SPS/PPS. Do not pass arbitrary avcC
        // records or guessed NAL length fields to the old binary worker.
        bool annexB = data.size() >= 4 && data[0] == 0 && data[1] == 0 &&
            (data[2] == 1 || (data[2] == 0 && data[3] == 1));
        if (!annexB || data.size() > MT6580_C2_MAX_CSD_BYTES) { fail(work, C2_BAD_VALUE); return; }
        if (std::search(mCsd.begin(), mCsd.end(), data.begin(), data.end()) == mCsd.end()) {
            if (data.size() > MT6580_C2_MAX_CSD_BYTES - mCsd.size()) { fail(work, C2_NO_MEMORY); return; }
            mCsd.insert(mCsd.end(), data.begin(), data.end());
        }
        status = transaction(MT6580_C2_CONFIG, token, pts, 0, data.data(), data.size(), work, pool);
        if (status == C2_OK && !eos) empty(work, false);
    } else if (!data.empty()) {
        if (mPending.size() >= 64 || !mPending.emplace(token, work->input.ordinal).second) {
            fail(work, C2_BAD_VALUE); return;
        }
        status = transaction(MT6580_C2_AU, token, pts, 0, data.data(), data.size(), work, pool, eos);
    } else if (!eos) empty(work, false);
    work->input.buffers.clear(); // worker owns copies, never C2 input references
    if (status == C2_NOT_FOUND) {
        mPending.clear(); mDeferred.reset(); empty(work, false);
        work->result = C2_NOT_FOUND; return;
    }
    if (status != C2_OK) { fail(work, status); return; }
    if (eos) {
        status = transaction(MT6580_C2_DRAIN, mControlToken--, pts, MT6580_C2_FLAG_EOS,
            nullptr, 0, work, pool, true);
        if (status == C2_NOT_FOUND) {
            mPending.clear(); mDeferred.reset(); empty(work, false);
            work->result = C2_NOT_FOUND; return;
        }
        if (status != C2_OK) { fail(work, status); return; }
        finishEmptyPending(work);
        empty(work, true);
        mEos = true;
    }
}
c2_status_t C2Mt6580AvcDec::drain(uint32_t mode, const std::shared_ptr<C2BlockPool>& pool) {
    if (mode == NO_DRAIN) return C2_OK;
    if (mode == DRAIN_CHAIN) return C2_OMITTED;
    if (mError || !mClient.connected()) return C2_BAD_STATE;
    bool eos = mode == DRAIN_COMPONENT_WITH_EOS;
    const std::unique_ptr<C2Work> noWork;
    c2_status_t status = transaction(MT6580_C2_DRAIN, mControlToken--, 0,
        eos ? MT6580_C2_FLAG_EOS : 0, nullptr, 0, noWork, pool, false, true);
    if (status == C2_NOT_FOUND) { mPending.clear(); mDeferred.reset(); return status; }
    if (status != C2_OK) { fail(noWork, status); return status; }
    finishEmptyPending(noWork, C2_OK, mDeferred ? mDeferred->token : UINT64_MAX);
    if (mDeferred) { emit(*mDeferred, noWork, false, eos); mDeferred.reset(); }
    if (eos) { mEos = true; return C2_OK; }
    // The legacy flush marks its decoder EOF. Reinitialize from retained CSD
    // before accepting more input for a drain-without-EOS request.
    return resetWorker(true);
}
}
