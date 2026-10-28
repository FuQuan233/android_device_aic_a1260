/* SPDX-License-Identifier: Apache-2.0 */
#include "Mt6580ComponentStore.h"
#include "C2Mt6580AvcDec.h"
#include "C2Mt6580Mpeg4Enc.h"
#include <C2PlatformSupport.h>
#include <C2Config.h>
#include <system/graphics.h>

namespace android {
namespace {
class StoreParams final : public C2InterfaceHelper {
public:
    explicit StoreParams(const std::shared_ptr<C2ReflectorHelper>& reflector)
        : C2InterfaceHelper(reflector) {
        reflector->addStructDescriptors<C2FlexiblePixelFormatDescriptorStruct>();
        // Minigbm's MTK backend supports YV12 CPU mappings. Flexible format 35
        // selects NV12, which this backend cannot allocate with CPU usage.
        auto formats = C2StoreFlexiblePixelFormatDescriptorsInfo::AllocShared(1u);
        formats->m.values[0] = {HAL_PIXEL_FORMAT_YV12, 8u, C2Color::YUV_420,
            C2Color::PLANAR_PACKED};
        addParameter(DefineParam(mFormats, "flexible-pixel-format-descriptors")
            .withConstValue(formats).build());
    }
private:
    std::shared_ptr<C2StoreFlexiblePixelFormatDescriptorsInfo> mFormats;
};
bool isLocalParam(C2Param::Index index) {
    return index == C2Param::Index(C2StoreFlexiblePixelFormatDescriptorsInfo::PARAM_TYPE);
}
void mergeStatus(c2_status_t status, c2_status_t* result) {
    if (*result == C2_OK || status == C2_NO_MEMORY) *result = status;
}
}
Mt6580ComponentStore::Mt6580ComponentStore()
    : mPlatform(GetCodec2PlatformComponentStore()),
      mReflector(std::static_pointer_cast<C2ReflectorHelper>(mPlatform->getParamReflector())) {
    mStoreParams = std::make_shared<StoreParams>(mReflector);
    auto add = [this](const char* name, C2Component::kind_t kind, const char* mime) {
        auto trait = std::make_shared<C2Component::Traits>();
        trait->name = name;
        trait->domain = C2Component::DOMAIN_VIDEO;
        trait->kind = kind;
        trait->mediaType = mime;
        trait->rank = 256;
        mTraits.push_back(trait);
    };
    add(C2Mt6580AvcDec::kName, C2Component::KIND_DECODER, "video/avc");
    add(C2Mt6580Mpeg4Enc::kMpeg4Name, C2Component::KIND_ENCODER, "video/mp4v-es");
    add(C2Mt6580Mpeg4Enc::kH263Name, C2Component::KIND_ENCODER, "video/3gpp");
}
C2String Mt6580ComponentStore::getName() const { return "mt6580"; }
static bool isEncoderName(const C2String& name) {
    return name == C2Mt6580Mpeg4Enc::kMpeg4Name || name == C2Mt6580Mpeg4Enc::kH263Name;
}
c2_status_t Mt6580ComponentStore::createComponent(C2String name,
        std::shared_ptr<C2Component>* component) {
    if (!component) return C2_BAD_VALUE;
    component->reset();
    const bool decoder = name == C2Mt6580AvcDec::kName;
    if (!decoder && !isEncoderName(name)) return C2_NOT_FOUND;
    std::lock_guard<std::mutex> lock(mMutex);
    // One decoder and one encoder worker may coexist. Native jobs share the
    // same serialized hardware owner; the two encoder names share one worker.
    auto& active = decoder ? mActiveDecoder : mActiveEncoder;
    if (!active.expired()) return C2_NO_MEMORY;
    c2_node_id_t id = mNextId.fetch_add(1);
    if (!id) return C2_NO_MEMORY;
    c2_status_t status = decoder ?
            C2Mt6580AvcDec::CreateComponent(id, mReflector, component) :
            C2Mt6580Mpeg4Enc::CreateComponent(name == C2Mt6580Mpeg4Enc::kH263Name,
                    id, mReflector, component);
    if (status == C2_OK) active = *component;
    return status;
}
c2_status_t Mt6580ComponentStore::createInterface(C2String name,
        std::shared_ptr<C2ComponentInterface>* interface) {
    if (!interface) return C2_BAD_VALUE;
    interface->reset();
    const bool decoder = name == C2Mt6580AvcDec::kName;
    if (!decoder && !isEncoderName(name)) return C2_NOT_FOUND;
    c2_node_id_t id = mNextId.fetch_add(1);
    if (!id) return C2_NO_MEMORY;
    return decoder ? C2Mt6580AvcDec::CreateInterface(id, mReflector, interface) :
            C2Mt6580Mpeg4Enc::CreateInterface(name == C2Mt6580Mpeg4Enc::kH263Name,
                    id, mReflector, interface);
}
std::vector<std::shared_ptr<const C2Component::Traits>> Mt6580ComponentStore::listComponents() {
    return mTraits;
}
c2_status_t Mt6580ComponentStore::copyBuffer(std::shared_ptr<C2GraphicBuffer>,
        std::shared_ptr<C2GraphicBuffer>) { return C2_OMITTED; }
c2_status_t Mt6580ComponentStore::query_sm(const std::vector<C2Param*>& params,
        const std::vector<C2Param::Index>& indices,
        std::vector<std::unique_ptr<C2Param>>* values) const {
    c2_status_t result = C2_OK;
    std::vector<C2Param*> local, platform;
    for (C2Param* param : params) {
        if (!param) return C2_BAD_VALUE;
        (isLocalParam(param->index()) ? local : platform).push_back(param);
    }
    if (!local.empty()) mergeStatus(mStoreParams->query(local, {}, C2_MAY_BLOCK, values), &result);
    if (!platform.empty()) mergeStatus(mPlatform->query_sm(platform, {}, values), &result);
    // Preserve the requested heap-index ordering even for mixed queries.
    for (C2Param::Index index : indices) {
        if (!values) return C2_BAD_VALUE;
        mergeStatus(isLocalParam(index) ?
            mStoreParams->query({}, {index}, C2_MAY_BLOCK, values) :
            mPlatform->query_sm({}, {index}, values), &result);
    }
    return result;
}
c2_status_t Mt6580ComponentStore::config_sm(const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* failures) {
    c2_status_t result = C2_OK;
    std::vector<C2Param*> local, platform;
    for (C2Param* param : params) {
        if (!param) return C2_BAD_VALUE;
        (isLocalParam(param->index()) ? local : platform).push_back(param);
    }
    if (!local.empty()) mergeStatus(mStoreParams->config(local, C2_MAY_BLOCK, failures), &result);
    if (!platform.empty()) mergeStatus(mPlatform->config_sm(platform, failures), &result);
    return result;
}
std::shared_ptr<C2ParamReflector> Mt6580ComponentStore::getParamReflector() const { return mReflector; }
c2_status_t Mt6580ComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* params) const {
    c2_status_t result = mPlatform->querySupportedParams_nb(params);
    mergeStatus(mStoreParams->querySupportedParams(params), &result);
    return result;
}
c2_status_t Mt6580ComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    c2_status_t result = C2_OK;
    for (auto& field : fields) {
        std::vector<C2FieldSupportedValuesQuery> one{field};
        c2_status_t status = mStoreParams->querySupportedValues(one, C2_MAY_BLOCK);
        if (one.front().status == C2_BAD_INDEX) status = mPlatform->querySupportedValues_sm(one);
        field = std::move(one.front());
        mergeStatus(status, &result);
    }
    return result;
}
}
