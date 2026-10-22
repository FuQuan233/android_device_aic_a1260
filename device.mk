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

# Rebuild vendor interfaces from this source tree; legacy binary HALs are not
# yet compatible with the mainline-style 6.18 hardware drivers.
$(call soong_config_set,minigbm,platform,generic)
PRODUCT_PACKAGES += \
    android.hardware.composer.hwc3-service.drm \
    android.hardware.graphics.allocator-service.minigbm \
    mapper.minigbm \
    vulkan.pastel \
    libEGL_angle \
    libGLESv1_CM_angle \
    libGLESv2_angle \
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
    ro.hardware.egl=angle \
    ro.hardware.vulkan=pastel \
    vendor.hwc.drm.device=/dev/dri/card0 \
    ro.vendor.hwc.use_overlay_planes=false \
    ro.opengles.version=196609 \
    persist.sys.usb.config=adb
PRODUCT_SYSTEM_PROPERTIES += \
    ro.radio.noril=true \
    ro.zygote=zygote32 \
    debug.renderengine.backend=skiaglthreaded
PRODUCT_SYSTEM_EXT_PROPERTIES += service.sf.prime_shader_cache=0
