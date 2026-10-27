/* SPDX-License-Identifier: Apache-2.0 */
#include "Mt6580ComponentStore.h"
#include "C2Mt6580AvcDec.h"
#include <C2PlatformSupport.h>

namespace android {
Mt6580ComponentStore::Mt6580ComponentStore()
    : mPlatform(GetCodec2PlatformComponentStore()),
      mReflector(std::static_pointer_cast<C2ReflectorHelper>(mPlatform->getParamReflector())),
      mTraits(std::make_shared<C2Component::Traits>()) {
    mTraits->name = C2Mt6580AvcDec::kName;
    mTraits->domain = C2Component::DOMAIN_VIDEO;
    mTraits->kind = C2Component::KIND_DECODER;
    mTraits->mediaType = "video/avc";
    mTraits->rank = 256;
}
C2String Mt6580ComponentStore::getName() const { return "mt6580"; }
c2_status_t Mt6580ComponentStore::createComponent(C2String name,
        std::shared_ptr<C2Component>* component) {
    if (!component) return C2_BAD_VALUE;
    component->reset();
    if (name != C2Mt6580AvcDec::kName) return C2_NOT_FOUND;
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mActive.expired()) return C2_NO_MEMORY;
    c2_node_id_t id = mNextId.fetch_add(1);
    if (!id) return C2_NO_MEMORY;
    c2_status_t status = C2Mt6580AvcDec::CreateComponent(id, mReflector, component);
    if (status == C2_OK) mActive = *component;
    return status;
}
c2_status_t Mt6580ComponentStore::createInterface(C2String name,
        std::shared_ptr<C2ComponentInterface>* interface) {
    if (!interface) return C2_BAD_VALUE;
    interface->reset();
    if (name != C2Mt6580AvcDec::kName) return C2_NOT_FOUND;
    c2_node_id_t id = mNextId.fetch_add(1);
    return id ? C2Mt6580AvcDec::CreateInterface(id, mReflector, interface) : C2_NO_MEMORY;
}
std::vector<std::shared_ptr<const C2Component::Traits>> Mt6580ComponentStore::listComponents() {
    return {mTraits};
}
c2_status_t Mt6580ComponentStore::copyBuffer(std::shared_ptr<C2GraphicBuffer>,
        std::shared_ptr<C2GraphicBuffer>) { return C2_OMITTED; }
c2_status_t Mt6580ComponentStore::query_sm(const std::vector<C2Param*>& params,
        const std::vector<C2Param::Index>& indices,
        std::vector<std::unique_ptr<C2Param>>* values) const {
    return mPlatform->query_sm(params, indices, values);
}
c2_status_t Mt6580ComponentStore::config_sm(const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* failures) {
    return mPlatform->config_sm(params, failures);
}
std::shared_ptr<C2ParamReflector> Mt6580ComponentStore::getParamReflector() const { return mReflector; }
c2_status_t Mt6580ComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* params) const {
    return mPlatform->querySupportedParams_nb(params);
}
c2_status_t Mt6580ComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    return mPlatform->querySupportedValues_sm(fields);
}
}
