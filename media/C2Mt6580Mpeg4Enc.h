/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <SimpleC2Component.h>
#include <util/C2InterfaceHelper.h>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace android {
/* The MT6580 native hardware encoder supports MPEG-4 Visual / H.263 here.
 * It must not be registered under an AVC name. The separate worker owns all
 * old-library pointers, native DMA buffers, pending frames and hardware jobs. */
class C2Mt6580Mpeg4Enc final : public SimpleC2Component {
public:
    class IntfImpl;
    static constexpr const char* kMpeg4Name = "c2.mt6580.mpeg4.encoder";
    static constexpr const char* kH263Name = "c2.mt6580.h263.encoder";
    static c2_status_t CreateComponent(bool h263, c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper,
        std::shared_ptr<C2Component>* component);
    static c2_status_t CreateInterface(bool h263, c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper,
        std::shared_ptr<C2ComponentInterface>* interface);
    C2Mt6580Mpeg4Enc(bool h263, c2_node_id_t id, const std::shared_ptr<IntfImpl>& intf);
    ~C2Mt6580Mpeg4Enc() override;
    c2_status_t flush_sm(flush_mode_t mode,
        std::list<std::unique_ptr<C2Work>>* const flushedWork) override;
    c2_status_t stop() override;
    c2_status_t reset() override;
    c2_status_t release() override;
protected:
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work>& work,
        const std::shared_ptr<C2BlockPool>& pool) override;
    c2_status_t drain(uint32_t mode, const std::shared_ptr<C2BlockPool>& pool) override;
private:
    class EncoderClient;
    struct Config {
        uint32_t codec, width, height, bitrate, fps, keyInterval, level;
        bool operator==(const Config& other) const;
    };
    struct Output {
        uint64_t token;
        C2WorkOrdinalStruct ordinal;
        std::shared_ptr<C2Buffer> buffer;
        std::vector<uint8_t> csd;
    };
    std::shared_ptr<IntfImpl> mIntf;
    std::unique_ptr<EncoderClient> mClient;
    const bool mH263;
    uint32_t mGeneration = 0;
    uint64_t mControlToken = UINT64_MAX;
    Config mConfig{};
    bool mConfigured = false, mError = false, mEos = false, mNeedsRestart = false;
    bool mCsdPending = false;
    std::vector<uint8_t> mCsd;
    std::map<uint64_t, C2WorkOrdinalStruct> mPending;
    std::unique_ptr<Output> mDeferred;
    std::mutex mOutputMutex;
    std::atomic<uint64_t> mAbortEpoch{0};
    uint64_t mTransactionEpoch = 0;
    c2_status_t mCallbackError = C2_OK;
    void cancelTransaction();
    c2_status_t connectWorker();
    c2_status_t ensureConfigured(const std::unique_ptr<C2Work>& current,
        const std::shared_ptr<C2BlockPool>& pool);
    c2_status_t transaction(uint32_t command, uint64_t token, int64_t pts,
        uint32_t flags, const void* data, size_t bytes,
        const std::unique_ptr<C2Work>& current, const std::shared_ptr<C2BlockPool>& pool,
        bool currentEos = false, bool deferLast = false);
    bool response(uint32_t command, uint64_t token, int64_t pts, uint32_t flags,
        const std::vector<uint8_t>& bytes, const std::unique_ptr<C2Work>& current,
        const std::shared_ptr<C2BlockPool>& pool, bool currentEos, bool deferLast);
    c2_status_t copyInput(const std::unique_ptr<C2Work>& current, std::vector<uint8_t>* i420);
    void emit(Output output, const std::unique_ptr<C2Work>& current,
        bool incomplete = false, bool eos = false);
    void empty(const std::unique_ptr<C2Work>& work, bool eos);
    void fail(const std::unique_ptr<C2Work>& current, c2_status_t status);
};
}
