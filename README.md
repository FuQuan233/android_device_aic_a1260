# AIC A1260 (wt6580_we_n) — LineageOS 24 / Android 17

This is a device product (`lineage_a1260`), with its own boot and vendor images.
It does not inherit a GSI, emulator BoardConfig, dynamic partitions or generic vendor.
The product uses full Lineage mobile configuration and the regular launcher,
with no Android Go/low-RAM defaults. This unit has no cellular baseband or
camera: it inherits AOSP's no-telephony base and Lineage's Wi-Fi-only overlay,
sets `ro.radio.noril=true`, and excludes Aperture with `PRODUCT_NO_CAMERA=true`.
Hardware feature overrides remove camera and cellular capability declarations.

`mt6580` identifies the SoC platform and Android hardware (`ro.hardware`),
including `init.mt6580.rc` and `fstab.mt6580`. `wt6580_we_n` identifies the
board variant, used by its DTS, compatible string and kernel defconfig.

## Local checkout and build

Place FuQuan233/android_kernel_aic_a1260, branch kernel-6.18, at
`kernel/aic/mt6580`. In the existing workspace that path is a local symlink to
`~/AIC/kernel7`. Android Clang r547379 is selected for the kernel.

Two local build-system fixes are saved under `patches/`. On a fresh checkout,
apply `lineage-kernel-module-detection.patch` from `vendor/lineage` and
`soong-memory-tuning.patch` from `build/soong` with `git apply`. The first
prevents `hardware=mt6580` in CONFIG_CMDLINE from being mistaken for a module.
The second forwards Go memory tuning into Soong's isolated environment.

    export OUT_DIR=out SOONG_NINJA=ninja GOGC=25 GOMEMLIMIT=22GiB
    source build/envsetup.sh
    lunch lineage_a1260-cp2a-userdebug
    m -j12 bootimage
    m -j12 systemimage vendorimage
    python3 device/aic/a1260/tools/validate_images.py out/target/product/a1260

The build output is `out/target/product/a1260`. The early bring-up target is
`bootimage`; check external ramdisk handover before installing system/vendor.
The stock boot and recovery are 16 MiB, header v0, page size 2048, kernel
0x80008000, ramdisk 0x84000000 and tags 0x8e000000. DTB is appended to zImage.
The image checker checks boot contents, filesystem superblocks, raw/sparse
expanded sizes against the fixed GPT limits, and records each image's SHA256.
It does not replace filesystem integrity checks or hardware boot validation.

## Actual storage and boot configuration

* system: mmcblk0p19, 1610612736 bytes, generated as compressed EROFS.
* vendor: mmcblk0p12, 427819008 bytes, newly built ext4 vendor image.
* cache: mmcblk0p20, 268435456 bytes, ext4.
* userdata: mmcblk0p21, 5317853184 bytes, ext4.
* product and system_ext live inside system. No super partition or A/B slots.

Data and cache fstab entries do not request formatting. Existing userdata has
not been migrated to Android 17 encryption or policy requirements. Original
shipping API is unknown and is intentionally not fabricated. This target is an
experimental port, not a claim of launch-device/VTS compliance.

The kernel profile keeps CPU0 only until secondary-core release is implemented.
SELinux is permissive during bring-up. Debug builds allow root ADB and a root
CDC console. The watchdog service keeps the 30-second hardware watchdog alive.
ADB/ACM configfs integration still requires hardware verification.
Post-fs-data configures 512 MiB of logical LZ4 zram swap. Pages consume RAM
as needed; no swap backing file or partition is written to eMMC.
Manual activation with Android 17 toybox has passed on hardware. A compressed
LZ4HC EROFS loop image also mounts and passes a 270000-byte read/SHA256 check.
The corresponding Android init actions and full system image are still untested.

## Hardware status

The kernel has tested USB CDC, eMMC and 480x800 JD9161 DRM scanout. The initial
Android graphics stack uses drm_hwcomposer, generic minigbm dumb buffers, ANGLE
and SwiftShader. These Android HALs have not yet been tested on this board.
Reference health/power services are used until PMIC/battery and CPU power
management drivers are ready. No legacy vendor binary HAL is assumed compatible.
Touch, hardware GPU acceleration, audio and Wi-Fi remain unfinished.
Camera and modem support are excluded because this unit has neither component.
The opt-in external ramdisk FDT handover has been hardware-tested with a
marker present only in the external archive. Android 17 static Bionic/toybox
also runs on hardware. Android PID 1 and the framework remain untested.

Software KeyMint and Gatekeeper are included for early userspace bring-up.
TEE-backed credential storage and production security policy remain unfinished.
