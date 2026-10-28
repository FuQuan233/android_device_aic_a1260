#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Defaults only: keep later user customizations and normal validation behavior.
set_default() {
    network_value=$(settings get global "$1") || return 1
    if [ "$network_value" = null ] || [ -z "$network_value" ]; then
        settings put global "$1" "$2"
    fi
}
set_default captive_portal_http_url http://connect.rom.miui.com/generate_204
set_default captive_portal_https_url https://connect.rom.miui.com/generate_204
set_default captive_portal_fallback_url http://connect.rom.miui.com/generate_204
set_default captive_portal_other_fallback_urls http://connect.rom.miui.com/generate_204
set_default ntp_server time.apple.com

# The framework software scan fallback requires both this DeviceConfig flag
# and the WifiCustomization resource overlay. Preserve explicit overrides.
software_pno_value=$(device_config get wifi software_pno_enabled) || exit 1
if [ "$software_pno_value" = null ] || [ -z "$software_pno_value" ]; then
    device_config put wifi software_pno_enabled true
fi
