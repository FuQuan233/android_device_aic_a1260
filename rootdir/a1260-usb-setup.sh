#!/vendor/bin/sh
set -e
G=/config/usb_gadget/g1
mkdir -p "$G"
echo 0x18d1 > "$G/idVendor"
echo 0x4ee7 > "$G/idProduct"
echo 0x0200 > "$G/bcdUSB"
echo 0 > "$G/bDeviceClass"
echo 0 > "$G/bDeviceSubClass"
echo 0 > "$G/bDeviceProtocol"
mkdir -p "$G/strings/0x409"
echo AIC > "$G/strings/0x409/manufacturer"
echo 'A1260 Android 17 bring-up' > "$G/strings/0x409/product"
# The original LK reads proinfo+0x74, at most 19 bytes, into a cleared
# 20-byte buffer. Decode only ASCII alphanumerics and stop at its first NUL.
# Keep binary MAC bytes and padding out of Android/USB identity strings.
a1260_proinfo=/dev/block/by-name/proinfo
# The factory nvdata backup identifies proinfo+0x6e as WIFI (record+4),
# and +0x68 as BT_Addr (record+0). Read only the six Wi-Fi address bytes.
a1260_read_factory_wifi_mac() {
    a1260_mac_hex=$(od -An -v -tx1 -j 110 -N 6 "$a1260_proinfo") || return 1
    set -- $a1260_mac_hex
    [ "$#" -eq 6 ] || return 1
    [ "$((0x$1 & 1))" -eq 0 ] || return 1
    a1260_mac=$(printf '%s:%s:%s:%s:%s:%s' "$1" "$2" "$3" "$4" "$5" "$6")
    [ "$a1260_mac" != 00:00:00:00:00:00 ] || return 1
    printf '%s' "$a1260_mac"
}
if a1260_mac=$(a1260_read_factory_wifi_mac); then
    if ! printf '%s\n' "$a1260_mac" > /sys/devices/platform/180f0000.wifi/factory_mac_address; then
        echo 'A1260 identity: failed to provision factory Wi-Fi MAC' > /dev/kmsg
    fi
else
    echo 'A1260 identity: no valid factory Wi-Fi MAC in proinfo' > /dev/kmsg
fi
a1260_read_factory_serial() {
    a1260_serial_hex=$(od -An -v -tx1 -j 116 -N 19 "$a1260_proinfo") || return 1
    set -- $a1260_serial_hex
    [ "$#" -eq 19 ] || return 1
    a1260_value=
    for a1260_byte do
        case "$a1260_byte" in
            00) break;;
            3[0-9]|4[1-9a-f]|5[0-9a]|6[1-9a-f]|7[0-9a]) ;;
            *) return 1;;
        esac
        a1260_escape=$(printf '\\%03o' "$((0x$a1260_byte))")
        a1260_value=$a1260_value$(printf '%b' "$a1260_escape")
    done
    [ -n "$a1260_value" ] || return 1
    printf '%s' "$a1260_value"
}
if a1260_serial=$(a1260_read_factory_serial); then
    for a1260_property in ro.boot.serialno ro.serialno; do
        a1260_existing=$(getprop "$a1260_property")
        if [ -z "$a1260_existing" ]; then
            setprop "$a1260_property" "$a1260_serial"
        elif [ "$a1260_existing" != "$a1260_serial" ]; then
            echo 'A1260 identity: boot serial conflicts with proinfo' > /dev/kmsg
        fi
    done
    setprop sys.usb.serialnumber "$a1260_serial"
else
    echo 'A1260 identity: no valid factory serial in proinfo' > /dev/kmsg
    a1260_serial=$(getprop ro.serialno)
fi
printf '%s\n' "$a1260_serial" > "$G/strings/0x409/serialnumber"
mkdir -p "$G/configs/b.1/strings/0x409"
echo 250 > "$G/configs/b.1/MaxPower"
mkdir -p "$G/functions/ffs.adb"
# Advertise WinUSB through the descriptors supplied by adbd's FunctionFS.
echo 1 > "$G/os_desc/use"
echo 0x1 > "$G/os_desc/b_vendor_code"
echo MSFT100 > "$G/os_desc/qw_sign"
ln -s "$G/configs/b.1" "$G/os_desc/b.1"
# init services inherit umask 077; shell adbd must traverse the parent too.
mkdir -p /dev/usb-ffs/adb
chown 2000:2000 /dev/usb-ffs /dev/usb-ffs/adb
chmod 0770 /dev/usb-ffs /dev/usb-ffs/adb
mount -t functionfs -o uid=2000,gid=2000,rmode=0770,fmode=0660 adb /dev/usb-ffs/adb
[ -e /dev/usb-ffs/adb/ep0 ]
echo 'A1260 USB: ADB FunctionFS mounted; ep0 present'
for controller in /sys/class/udc/*; do
    [ -d "$controller" ] || continue
    setprop sys.usb.controller "${controller##*/}"
    setprop sys.usb.configfs 1
    echo 'A1260 USB: controller selected; waiting for adbd descriptors'
    exit 0
done
echo 'A1260: no USB device controller' > /dev/kmsg
exit 1
