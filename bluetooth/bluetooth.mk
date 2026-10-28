$(call inherit-product-if-exists, vendor/aic/a1260-bluetooth/bluetooth-vendor.mk)

PRODUCT_PACKAGES += android.hardware.bluetooth-service.mt6580

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml
