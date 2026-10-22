#!/vendor/bin/sh
set -e
G=/config/usb_gadget/g1
mkdir -p "$G"
echo 0x0525 > "$G/idVendor"
echo 0xa4a7 > "$G/idProduct"
mkdir -p "$G/strings/0x409"
echo AIC > "$G/strings/0x409/manufacturer"
echo 'A1260 Android 17 bring-up' > "$G/strings/0x409/product"
serial=$(getprop ro.serialno)
[ -n "$serial" ] || serial=$(cat /sys/block/mmcblk0/device/cid)
echo "$serial" > "$G/strings/0x409/serialnumber"
mkdir -p "$G/configs/b.1/strings/0x409"
echo 250 > "$G/configs/b.1/MaxPower"
mkdir -p "$G/functions/acm.gs0" "$G/functions/ffs.adb"
mkdir -p /dev/usb-ffs/adb
mount -t functionfs -o uid=2000,gid=2000 adb /dev/usb-ffs/adb
for controller in /sys/class/udc/*; do
    [ -d "$controller" ] || continue
    setprop sys.usb.controller "${controller##*/}"
    setprop sys.usb.configfs 1
    exit 0
done
echo 'A1260: no USB device controller' > /dev/kmsg
exit 1
