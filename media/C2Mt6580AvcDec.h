/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "LegacyClient.h"
#include <SimpleC2Component.h>
#include <util/C2InterfaceHelper.h>
#include <atomic>
#include <map>
#include <vector>

namespace android {
class C2Mt6580AvcDec final : public SimpleC2Component {
public:
    class IntfImpl;
    static constexpr const char* kName = "c2.mt6580.avc.decoder";
    static c2_status_t CreateComponent(c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper,
        std::shared_ptr<C2Component>* component);
    static c2_status_t CreateInterface(c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper>& helper,
        std::shared_ptr<C2ComponentInterface>* interface);
    C2Mt6580AvcDec(c2_node_id_t id, const std::shared_ptr<IntfImpl>& interface);
    ~C2Mt6580AvcDec() override;
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
    struct Output {
        uint64_t token;
        C2WorkOrdinalStruct ordinal;
        std::shared_ptr<C2Buffer> buffer;
        uint32_t width, height;
        bool config;
    };
    std::shared_ptr<IntfImpl> mIntf;
    mt6580::LegacyClient mClient;
    uint32_t mGeneration = 1;
    uint64_t mControlToken = UINT64_MAX;
    uint32_t mWidth = 0, mHeight = 0;
    bool mHaveFormat = false, mFormatPending = false;
    bool mError = false, mEos = false, mNeedsRestart = false;
    std::mutex mOutputMutex;
    std::atomic<uint64_t> mAbortEpoch{0};
    uint64_t mTransactionEpoch = 0;
    void cancelTransaction();
    c2_status_t mCallbackError = C2_OK;
    std::vector<uint8_t> mCsd;
    std::map<uint64_t, C2WorkOrdinalStruct> mPending;
    std::unique_ptr<Output> mDeferred;
    c2_status_t transaction(uint32_t command, uint64_t token, int64_t pts,
        uint32_t flags, const void* bytes, size_t size,
        const std::unique_ptr<C2Work>& current, const std::shared_ptr<C2BlockPool>& pool,
        bool currentEos = false, bool deferLast = false);
    bool response(const mt6580_c2_header& header, const std::vector<uint8_t>& bytes,
        const std::unique_ptr<C2Work>& current, const std::shared_ptr<C2BlockPool>& pool,
        bool currentEos, bool deferLast);
    void emit(Output output, const std::unique_ptr<C2Work>& current,
        bool incomplete = false, bool eos = false);
    void empty(const std::unique_ptr<C2Work>& work, bool eos);
    void finishEmptyPending(const std::unique_ptr<C2Work>& current,
        c2_status_t error = C2_OK, uint64_t except = UINT64_MAX);
    void fail(const std::unique_ptr<C2Work>& current, c2_status_t error);
    c2_status_t resetWorker(bool replayCsd);
};
}
