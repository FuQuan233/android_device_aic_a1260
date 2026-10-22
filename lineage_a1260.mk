# Device-specific Android 17 bring-up for the AIC A1260.
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/go_defaults.mk)
PRODUCT_TYPE := go
ifneq ($(TARGET_BUILD_VARIANT),user)
WITH_ADB_INSECURE := true
endif
$(call inherit-product, vendor/lineage/config/common_mini_go_phone.mk)
$(call inherit-product, device/aic/a1260/device.mk)

PRODUCT_NAME := lineage_a1260
PRODUCT_DEVICE := a1260
PRODUCT_BRAND := AIC
PRODUCT_MODEL := A1260
PRODUCT_MANUFACTURER := SSH
PRODUCT_LOCALES := en_US zh_CN
