# AIDL Wi-Fi wraps the small, real nl80211 station HAL. The framework keeps
# scan/association/EAPOL in wificond and the current source wpa_supplicant.
$(call inherit-product-if-exists, vendor/aic/a1260-wifi/wifi-vendor.mk)

PRODUCT_PACKAGES += \
    A1260WifiOverlay \
    android.hardware.wifi-service \
    libwifi-hal-mt6580 \
    mt6580-wmt-dispatcher \
    wpa_supplicant \
    wpa_cli \
    iw_vendor

PRODUCT_VENDOR_PROPERTIES += wifi.interface=wlan0

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    device/aic/a1260/wifi/vendor_hal.xml:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/vendor_hals/mt6580.xml \
    device/aic/a1260/wifi/wpa_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant.conf \
    device/aic/a1260/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf \
    device/aic/a1260/wifi/init.mt6580.wifi.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.mt6580.wifi.rc \
    external/wireless-regdb/regulatory.db:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db \
    external/wireless-regdb/regulatory.db.p7s:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db.p7s

# Persist the requested defaults on existing bring-up images as well as fresh
# userdata; user-selected settings are preserved by the one-shot initializer.
PRODUCT_COPY_FILES += \
    device/aic/a1260/wifi/network-defaults.sh:$(TARGET_COPY_OUT_VENDOR)/etc/a1260-network-defaults.sh
