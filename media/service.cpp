/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "media.c2.mt6580"
#include "Mt6580ComponentStore.h"
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <codec2/aidl/ComponentStore.h>
#include <log/log.h>
#include <minijail.h>
#include <signal.h>

int main() {
    signal(SIGPIPE, SIG_IGN);
    // Device integration supplies the extension's Unix-socket syscall rules.
    android::SetUpMinijail(
        "/vendor/etc/seccomp_policy/android.hardware.media.c2-default-seccomp_policy",
        "/vendor/etc/seccomp_policy/codec2.vendor.ext.policy");
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    using aidl::android::hardware::media::c2::utils::ComponentStore;
    auto store = ndk::SharedRefBase::make<ComponentStore>(
        std::make_shared<android::Mt6580ComponentStore>());
    std::string name = std::string(ComponentStore::descriptor) + "/default";
    binder_status_t status = AServiceManager_addService(store->asBinder().get(), name.c_str());
    if (status != STATUS_OK) {
        ALOGE("Cannot register %s: %d", name.c_str(), status);
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 1;
}
