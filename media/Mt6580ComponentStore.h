/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <C2Component.h>
#include <util/C2InterfaceHelper.h>
#include <atomic>
#include <mutex>

namespace android {
class Mt6580ComponentStore final : public C2ComponentStore {
public:
    Mt6580ComponentStore();
    C2String getName() const override;
    c2_status_t createComponent(C2String name, std::shared_ptr<C2Component>* component) override;
    c2_status_t createInterface(C2String name, std::shared_ptr<C2ComponentInterface>* interface) override;
    std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override;
    c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer>, std::shared_ptr<C2GraphicBuffer>) override;
    c2_status_t query_sm(const std::vector<C2Param*>& params,
        const std::vector<C2Param::Index>& indices,
        std::vector<std::unique_ptr<C2Param>>* values) const override;
    c2_status_t config_sm(const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* failures) override;
    std::shared_ptr<C2ParamReflector> getParamReflector() const override;
    c2_status_t querySupportedParams_nb(std::vector<std::shared_ptr<C2ParamDescriptor>>* params) const override;
    c2_status_t querySupportedValues_sm(std::vector<C2FieldSupportedValuesQuery>& fields) const override;
private:
    std::shared_ptr<C2ComponentStore> mPlatform;
    std::shared_ptr<C2ReflectorHelper> mReflector;
    std::shared_ptr<C2InterfaceHelper> mStoreParams;
    std::vector<std::shared_ptr<const C2Component::Traits>> mTraits;
    std::mutex mMutex;
    std::weak_ptr<C2Component> mActiveDecoder, mActiveEncoder;
    std::atomic<c2_node_id_t> mNextId{1};
};
}
