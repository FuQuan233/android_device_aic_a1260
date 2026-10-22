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
serial=$(getprop ro.serialno)
[ -n "$serial" ] || serial=$(cat /sys/block/mmcblk0/device/cid)
echo "$serial" > "$G/strings/0x409/serialnumber"
mkdir -p "$G/configs/b.1/strings/0x409"
echo 250 > "$G/configs/b.1/MaxPower"
mkdir -p "$G/functions/ffs.adb"
# Advertise WinUSB through the descriptors supplied by adbd's FunctionFS.
echo 1 > "$G/os_desc/use"
echo 0x1 > "$G/os_desc/b_vendor_code"
echo MSFT100 > "$G/os_desc/qw_sign"
ln -s "$G/configs/b.1" "$G/os_desc/b.1"
mkdir -p /dev/usb-ffs/adb
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
