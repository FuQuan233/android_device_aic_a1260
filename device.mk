LOCAL_PATH := device/aic/a1260
PRODUCT_SOURCE_ROOT_DIRS += -kernel/aic/mt6580
TARGET_SUPPORTS_32_BIT_APPS := true
TARGET_SUPPORTS_64_BIT_APPS := false
PRODUCT_USE_DYNAMIC_PARTITIONS := false
PRODUCT_BUILD_SUPER_PARTITION := false
AB_OTA_UPDATER := false
PRODUCT_BUILD_BOOT_IMAGE := true
PRODUCT_BUILD_VENDOR_IMAGE := true
PRODUCT_BUILD_RECOVERY_IMAGE := false
PRODUCT_USE_DYNAMIC_PARTITION_SIZE := false
USE_SOONG_DEFINED_SYSTEM_IMAGE := false
PRODUCT_AAPT_CONFIG := normal hdpi
PRODUCT_AAPT_PREF_CONFIG := hdpi
PRODUCT_SOONG_NAMESPACES += $(LOCAL_PATH)

# Standard 1 GiB heap profile; avoid ART falling back to a 16 MiB heap.
$(call inherit-product, frameworks/native/build/phone-xhdpi-1024-dalvik-heap.mk)

# Rebuild vendor interfaces from this source tree; legacy binary HALs are not
# yet compatible with the mainline-style 6.18 hardware drivers.
$(call soong_config_set,minigbm,platform,generic)
# Only this debug port opts into native ARM32 networking on Linux 6.18.
$(call soong_config_set,aic_a1260,mt6580_arm32_kernel,true)
PRODUCT_PACKAGES += \
    android.hardware.composer.hwc3-service.drm \
    android.hardware.graphics.allocator-service.minigbm \
    mapper.minigbm \
    gralloc.minigbm \
    libEGL_mesa \
    libGLESv1_CM_mesa \
    libGLESv2_mesa \
    a1260-gpu-info \
    vulkan.pastel \
    libEGL_angle \
    libGLESv1_CM_angle \
    libGLESv2_angle \
    com.android.hardware.audio \
    android.hardware.health-service.example \
    android.hardware.power-service.example \
    android.hardware.security.keymint-service.nonsecure \
    com.android.hardware.gatekeeper.nonsecure \
    watchdogd \
    sh_vendor \
    toybox_vendor \
    a1260-usb-setup

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/handheld_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/handheld_core_hardware.xml \
    $(LOCAL_PATH)/permissions/a1260-absent-hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/a1260-absent-hardware.xml \
    $(LOCAL_PATH)/rootdir/fstab.mt6580:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.mt6580 \
    $(LOCAL_PATH)/rootdir/fstab.mt6580:$(TARGET_COPY_OUT_RAMDISK)/fstab.mt6580 \
    $(LOCAL_PATH)/rootdir/init.mt6580.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.mt6580.rc \
    $(LOCAL_PATH)/rootdir/init.mt6580.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.mt6580.usb.rc \
    $(LOCAL_PATH)/rootdir/ueventd.mt6580.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc

PRODUCT_VENDOR_PROPERTIES += \
    ro.sf.lcd_density=240 \
    ro.hardware.gralloc=minigbm \
    ro.hardware.egl=mesa \
    ro.hardware.vulkan=pastel \
    vendor.hwc.drm.device=/dev/dri/card% \
    ro.vendor.hwc.use_overlay_planes=false \
    ro.opengles.version=131072 \
    persist.sys.usb.config=adb
PRODUCT_SYSTEM_PROPERTIES += \
    ro.radio.noril=true \
    ro.zygote=zygote32 \
    debug.renderengine.backend=skiaglthreaded
PRODUCT_SYSTEM_EXT_PROPERTIES += service.sf.prime_shader_cache=0

# Reference AIDL audio services keep AudioFlinger available during bring-up.
# Physical MT6580 audio is not implemented; the boot flags select stub I/O.
$(call inherit-product, hardware/interfaces/audio/aidl/default/audio_effects.mk)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/bluetooth_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml
