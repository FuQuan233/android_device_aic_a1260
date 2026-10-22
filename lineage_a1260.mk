# Device-specific Android 17 bring-up for the AIC A1260.
PRODUCT_NO_CAMERA := true
TARGET_NO_TELEPHONY := true
$(call inherit-product, $(SRC_TARGET_DIR)/product/generic_no_telephony.mk)
ifneq ($(TARGET_BUILD_VARIANT),user)
WITH_ADB_INSECURE := true
endif
$(call inherit-product, vendor/lineage/config/common_mobile_full.mk)
$(call inherit-product, vendor/lineage/config/wifionly.mk)
$(call inherit-product, device/aic/a1260/device.mk)

# Non-GKI ARMv7: compile images and runtime consistently for ART CC GC.
PRODUCT_ENABLE_UFFD_GC := false

PRODUCT_NAME := lineage_a1260
PRODUCT_DEVICE := a1260
PRODUCT_BRAND := AIC
PRODUCT_MODEL := A1260
PRODUCT_MANUFACTURER := SSH
PRODUCT_LOCALES := en_US zh_CN
PRODUCT_CHARACTERISTICS := default
