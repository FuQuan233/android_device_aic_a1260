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

Local source fixes are saved under `patches/`. On a fresh checkout,
apply `lineage-kernel-module-detection.patch` from `vendor/lineage` and
`soong-memory-tuning.patch` from `build/soong` with `git apply`. The first
prevents `hardware=mt6580` in CONFIG_CMDLINE from being mistaken for a module.
The second forwards Go memory tuning into Soong's isolated environment.
Apply `mt6580-native-arm32-netbpfload.patch` from
`packages/modules/Connectivity` for this native ARM32 port. The device opts in
through `aic_a1260.mt6580_arm32_kernel`; runtime exemptions apply only to
mt6580, ARM32 kernel/userspace, Linux 6.18 and userdebug/eng builds. Upstream
architecture policy exits are relaxed, while real BPF syscall, program/map
loading and verifier failures remain fatal. Network functionality and ABI
compatibility still require hardware testing; this is not VTS compliance.

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

The fstab uses `/dev/block/by-name/system`, `vendor`, `cache` and `userdata`.
Android 17 first-stage discovery matches GPT PARTNAME before the kernel device
name, so numbered paths like `mmcblk0p19` fail discovery even when eMMC and
its GPT partitions are present. `androidboot.boot_devices=11120000.mmc`
enables the global by-name links for this controller.

Data and cache fstab entries do not request formatting. Existing userdata has
not been migrated to Android 17 encryption or policy requirements. Original
shipping API is unknown and is intentionally not fabricated. This target is an
experimental port, not a claim of launch-device/VTS compliance.

The kernel profile keeps CPU0 only until secondary-core release is implemented.
SELinux is permissive during bring-up. Debug builds allow root ADB; the physical UART retains early kernel logs. The watchdog service keeps the 30-second hardware watchdog alive.
The Android image exposes only FunctionFS/ADB, without ACM or ttyGS0.
ADB configfs integration still requires hardware verification.
The fs action configures 512 MiB of logical LZ4 zram swap before ART starts.
Android's ordinary overcommit policy is applied in init for the same reason. Pages consume RAM
as needed; no swap backing file or partition is written to eMMC.
Manual activation with Android 17 toybox and activation by Android init have
passed on hardware. A compressed LZ4HC EROFS loop image also mounts and passes
a 270000-byte read/SHA256 check. The full system image mounts successfully.

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
also runs on hardware. Android first- and second-stage init run on hardware;
system/vendor and data/cache mount successfully. Framework/UI startup and ADB
remain under bring-up. APEX dm-verity requires registered Crypto API SHA256
support even when board-level AVB is disabled; the kernel profile now includes
SHA256/SHA512 and the boot validator checks both. Hardware now activates all
40 APEX packages. The opt-in patch above passes the ARM32 architecture
policy checks on hardware. Functional BPF loading then found upstream ARM32
JIT missing atomic opcode 0xc3; kernel commit a3350b4e7 adds native ARMv7
atomic emission, with emulator execution tests. On-device loading with this
kernel fix now passes the atomic instructions on hardware: offload, test and
CLAT objects load before the next DSCP failure. ARM32 JIT cannot link BPF
subprograms/callbacks, so the device-scoped loader patch selects the existing
inline DSCP/netd objects instead of their `@funcs` variants. Maps, network
policies and actual loading/verifier/JIT checks remain enabled. This choice
is now hardware-tested: NetBpfLoad reports success and exits with status 0.
The previous blocker was netd repeatedly exiting with status 1 and
restarting zygote. The expanded kernel configuration now keeps netd running
on hardware without that restart loop. A source audit found missing raw tables, connection marks,
REJECT, TCPMSS, policy routing and XFRM support in the kernel profile; these
are enabled and checked by the boot validator. Early zram activation is hardware-tested
and the prior ART allocation failure is no longer present in UART output.
The metadata partition is not mounted yet; aconfig reports read-only metadata
storage and remains a separate follow-up issue.

Software KeyMint and Gatekeeper are included for early userspace bring-up.
TEE-backed credential storage and production security policy remain unfinished.

During debug bring-up a filtered main/system/crash logcat reader sends errors
and Netd/adbd messages to kmsg/UART. The kernel log buffer is excluded to
avoid feedback. USB uses ADB only (18d1:4ee7), per-interface device class,
WinUSB OS descriptors and explicit FunctionFS permissions. The common USB rc
links ffs.adb and binds the UDC only when adbd provides descriptors. CDC/ACM,
its console service and ttyGS0 kernel command line are removed. Setup and
FunctionFS readiness markers are written to the UART. The expanded netd configuration is hardware-tested. The next full UART log
shows adbd starting only its TCP/vsock backend: init's inherited umask 077
left /dev/usb-ffs inaccessible to shell after adbd dropped privileges. Setup
now explicitly assigns shell ownership and mode 0770 to both parent and mount
directories; USB enumeration needs another hardware boot to confirm.

The same log shows audioserver repeatedly dereferencing a missing audio HAL.
The reference AIDL audio APEX, effects and policy XML are now included. Primary
streams simulate input and discard output through the reference HAL's boot
flags until a real MT6580 ALSA driver is ready. These flags are present in
both the boot header command line and the compiled kernel command line,
because the appended-DTB handover does not import LK bootargs. This enables
software audio services, not physical microphone or speaker operation.


## Framework boot fixes after USB ADB became available

USB ADB shell, reconnect and UID 0 shell are verified on hardware. The reference
AIDL core/effect audio services register and audioserver remains running.
System server starts, but completion is still under investigation.

ART's shipped boot image uses CMC while a kernel without USERFAULTFD makes the
runtime fall back to CC/read barriers. The kernel Android profile now enables
USERFAULTFD so ART can use the image. The standard 1 GiB Dalvik heap profile
replaces the unset 16 MiB default.

The early-only mount_all pass suppressed nonencrypted, leaving installd and
other main/late_start services stopped. Userdata now uses latemount and a
late-fs mount_all pass queues the normal startup event. Metadata is mounted
in first stage so aconfig can initialize before Java services read flags.
The 41287680-byte previously unformatted metadata partition was backed up to
/home/fuquan/AIC/out/wt6580-reference/metadata-before-android17.img and initialized
as ext4 explicitly during bring-up; the fstab does not format it or userdata.
UART logcat forwarding stops when USB ADB is available to reduce CPU load.

NetworkStatsService also needs CONFIG_NET_KEY: BpfNetMaps synchronizes RCU by
opening/closing a PF_KEY socket. Without it, errno 97 propagates as a fatal
system_server exception. The Android kernel profile now includes PF_KEY.
Hardware also confirmed the 16 MiB heap eventually causes system_server OOM,
so the 1 GiB heap profile is required rather than just a startup optimization.


The UFFD/CMC hardware boot exposed a separate kernel failure: pstore records
ext4_dirty_folio warnings and a NULL call from unmap_page_range during exec
in a UID 9999 child, followed by a panic and reboot at about 38 seconds.
A zygote SIGBUS was also observed. These findings do not establish that USB
FIFO sizing caused the crash. Evidence is saved in
/home/fuquan/AIC/out/a1260-build-logs/boot-cmc-previous-pstore.log.

The product now explicitly uses PRODUCT_ENABLE_UFFD_GC=false, the supported
non-GKI ART CC configuration. Boot images and all dexpreopt outputs must be
rebuilt with the runtime property, rather than mixing CMC images and CC.
This changes GC configuration only; the full non-Go product remains enabled.
Hardware completion is still pending.

Android has not displayed a boot animation on the physical LCD. An active
bootanimation process is not evidence of scanout. HWC logs show
`DRM/KMS master access required`, followed by a headless null display; DRM
state also shows the CRTC disabled and no framebuffer. The DRM clients table
shows the graphics allocator holding master rather than the compositor.
This display-only driver has no render node, so cros_gralloc falls back to
the primary node and implicitly becomes master. Apply
`patches/minigbm-release-allocator-drm-master.patch` from `external/minigbm`
to release that ownership after allocator initialization, matching minigbm's
existing GBM discovery behavior. Physical Android scanout remains unverified.

That allocator fix is now verified: HWC holds DRM master, attaches DSI-1,
and the user sees the LineageOS boot animation on the actual LCD. RDMA reports
no underflows. CC also reaches `sys.boot_completed=1`, but Launcher/SystemUI
still crash when the 16 MiB CMA pool is exhausted; this does not yet qualify
as a working desktop. The kernel increases the reusable display CMA pool to
64 MiB for the next hardware boot.

## Lima GPU bring-up

The kernel now describes the Mali-400 at 0x13010000, its six interrupts, MFG
power domain and clock gates using the MT6580 vendor platform/clock/power
sources as reference. The top clock mux and supply voltages remain as firmware
left them; DVFS is not enabled. The first Lima kernel and ARMv7 Mesa libraries
compile, but actual GPU identification, completed jobs and Android rendering
still need hardware verification. The vendor product selects Mesa EGL/GLES2
and retains SwiftShader Vulkan/ANGLE libraries for bring-up fallback. Mali-400
does not provide Vulkan or GLES3; the advertised GLES version is 2.0.

Mesa prebuilts are rebuilt from this workspace's `external/mesa3d` with:

    PYTHONPATH=/path/to/python-tools python3 device/aic/a1260/tools/build_lima_mesa.py \
        --toolchain /path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64 \
        --out /path/to/mesa-lima -j 8

The Python tools directory needs meson >= 1.4, mako, packaging and PyYAML.
The build uses this product's vendor libdrm; Android stub libraries are only
link-time interfaces and are never installed. Meson installation removes
build RPATHs before copying the real EGL/GLES/Gallium libraries into the device
prebuilts directory. The accompanying NDK libc++_shared is separate from the
platform libc++. `a1260-gpu-info` reports the EGL/GL renderer and submits an
offscreen triangle with readback, allowing GPU completion to be checked even
if the Android window stack fails.

The current system can enter s2idle and fail to wake. A temporary kernel
wake lock keeps bring-up sessions usable. The user's explicit removal request,
the exact command and the resume verification conditions are recorded in
`TODO.md`; the lock must be removed once the wake path is repaired.
