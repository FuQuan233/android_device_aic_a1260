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

Apply `health-loop-drain-before-rearm.patch` from `hardware/interfaces`.
The health event loop must drain ready timerfd expirations before resetting
its slow wake timer; otherwise timerfd_settime clears the count before read.
The patch also retries interrupted reads and accepts a stale nonblocking
read when another event in the batch changed the charger interval.

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

The kernel profile boots all four Cortex-A7 cores through the MT6580 SPM
power sequence and non-secure boot vector. Physical ARM timers avoid the
timekeeping stalls observed with virtual timers on this firmware.
Hardware verifies all four CPUs online, local timer interrupts, correct
shared atomic updates, monotonic time across CPU migrations and 3.98-way
parallel execution. CPU hot-unplug, shared CPU frequency scaling and light s2idle have since been implemented; see the CPU power-management notes in TODO.md.
SELinux is permissive during bring-up. Normal userdebug startup uses ADB shell with on-demand `adb root`; the physical UART retains early kernel logs. The watchdog service keeps the 30-second hardware watchdog alive.
The Android image exposes only FunctionFS/ADB, without ACM or ttyGS0.
USB ADB enumeration and root shell are verified; large bulk transfers remain unreliable.
The fs action configures 512 MiB of logical LZ4 zram swap before ART starts.
Android's ordinary overcommit policy is applied in init for the same reason. Pages consume RAM
as needed; no swap backing file or partition is written to eMMC.
Manual activation with Android 17 toybox and activation by Android init have
passed on hardware. A compressed LZ4HC EROFS loop image also mounts and passes
a 270000-byte read/SHA256 check. The full system image mounts successfully.

## Hardware status

The kernel has tested USB CDC, eMMC and 480x800 JD9161 DRM scanout. The Android graphics stack uses drm_hwcomposer, minigbm KMS buffers and
the original Mali r8p0 GLES2 stack described below. Physical boot animation,
desktop, Settings and GPU drawing/readback are verified.
The standard Health service now reads the MT6350 AUXADC battery and BQ24296
charger through power_supply. Battery presence, voltage, temperature and real
USB charging/discharging status are verified. Capacity uses the reference BSP
voltage curves and still needs cell/load calibration. The PMIC power key is
also connected; the reference Power service remains until CPU power management
is ready. Proprietary graphics userspace is handled by the integration below.
Touch is verified; hardware audio and Wi-Fi remain unfinished.
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
Metadata is now initialized and mounted; aconfig initialization passes.

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
directories; hardware now enumerates ADB and provides a working shell.

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
System server reaches boot_completed; desktop and Settings rendering are verified.

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
The matching CC system is flashed and passes the earlier panic point; a working
desktop remains the completion criterion.

Before the allocator ownership fix, Android did not display a boot animation
on the physical LCD. HWC logs showed
`DRM/KMS master access required`, followed by a headless null display; DRM
state also shows the CRTC disabled and no framebuffer. The DRM clients table
shows the graphics allocator holding master rather than the compositor.
This display-only driver has no render node, so cros_gralloc falls back to
the primary node and implicitly becomes master. Apply
`patches/minigbm-mt6580-display-and-lima.patch` from `external/minigbm`
to release that ownership after allocator initialization, matching minigbm's
existing GBM discovery behavior.

That allocator fix is now verified: HWC holds DRM master, attaches DSI-1,
and the user sees the LineageOS boot animation on the actual LCD. RDMA reports
no underflows. CC also reaches `sys.boot_completed=1`, but Launcher/SystemUI
still crash when the 16 MiB CMA pool is exhausted; this does not yet qualify
as a working desktop. The kernel increases the reusable display CMA pool to
64 MiB; the new pool is active on hardware.

## Lima GPU bring-up

The Lima work below is the previous graphics configuration. The current product
selects the proprietary r8p0 userspace and GPL Utgard kernel driver described
in the following section; both kernel drivers must not be enabled together.

The kernel now describes the Mali-400 at 0x13010000, its six interrupts, MFG
power domain and clock gates using the MT6580 vendor platform/clock/power
sources as reference. The top clock mux and supply voltages remain as firmware
left them; DVFS is not enabled. Hardware identifies Mali-400 MP2 at the firmware
500.5 MHz setting. An offscreen triangle completes with the expected green
pixel and no GL error; SurfaceFlinger reports Mesa/Mali400, and the user
confirms normal, much smoother animation. Application buffer import now also
passes: the user sees the desktop, and Settings renders its text and icons.
Launcher and SystemUI remain alive after startup. The vendor product selects Mesa EGL/GLES2
and retains SwiftShader Vulkan/ANGLE libraries for bring-up fallback. Mali-400
does not provide Vulkan or GLES3; the advertised GLES version is 2.0.

The minigbm patch above adds an import-only Lima backend using PRIME and the
Lima GEM mmap interface. Mapper/legacy metadata queries use the render node;
AIDL/Gralloc4 allocation skips import-only backends and retains KMS allocation
for contiguous scanout. Applications can open render nodes (0666), while
primary card nodes remain limited to system/graphics (0660). Five matching
vendor minigbm modules were deployed atomically through root ADB, followed by
a hardware reboot. The vendor image includes the same fix.

The previous single-core profile triggered SystemUI ANRs during cold startup.
The four-core physical-timer build reaches the desktop without SystemUI ANRs
in the captured boot events; long-term startup performance remains to be assessed.
Bluetooth separately aborts because
real-time scheduling is unavailable. Neither issue is treated as a GPU fix.
SELinux remains permissive; physical audio/Wi-Fi are still pending.

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


## Original Mali r8p0 integration

The product now selects `CONFIG_MALI400=y`, `CONFIG_DRM_LIMA=n`,
`ro.hardware.egl=mali` and the minigbm `mt6580-utgard` platform. The original
owner-supplied Android 8.1 blobs are packaged locally, with private SONAMEs and
matching dependency names. Current Bionic, libc++, liblog and libvndksupport
remain provided by Android 17. Proprietary binaries and device efuses are not
part of this device repository.

On a fresh source tree, apply the updated
`patches/minigbm-mt6580-display-and-lima.patch` in `external/minigbm` (it includes
the display, Lima and conditional Utgard changes), and
`patches/drm-hwcomposer-mt6580-utgard.patch` in `external/drm_hwcomposer`.
The latter selects this device's image-FD adapter only for `mt6580-utgard`.
It also supports `vendor.hwc.drm.backlight_min`, set to 4 by this device's init:
Composer's 0.0 is minimum visible brightness, while negative/off is handled by
`bl_power`. The active PWM range is 4..255 and screen-off retains zero duty;
the Android brightness slider and framework minimum are unchanged.

Prepare the local vendor package before building:

    python3 device/aic/a1260/tools/prepare_stock_mali.py \
        /path/to/extracted-stock-libraries vendor/aic/a1260 \
        --patchelf /path/to/patchelf \
        --devinfo-atag /path/to/same-device/device-tree/chosen/atag,devinfo

The input directory contains `lib/*.so` and `lib/egl/libGLES_mali.so`. The ATAG
must come from this device's original kernel backup. The script validates its
length, tag, word count and MT6580 hardware code, then converts the original
binary data to the legacy count-plus-efuse format. It adapts the private
`libmtk_drvb` filename to `/vendor/etc/mt6580-devinfo.bin`; it does not replace
validation functions or force successful results. Without this real input the
original helper returns `0x1004`, then the blob deliberately writes
`0xddeeaadd` beyond its stack frame, causing unrelated-looking EGL, JNI and
stack-canary crashes. The missing legacy sysfs input was confirmed by runtime
logs and an LLDB breakpoint in `drvb_error`.

The minigbm bridge keeps modern mapper metadata but uses the exact 168-byte
original ARM32 RGB handle layout: two FDs, 37 integers, metadata FD first and
image DMA-BUF second. KMS/CMA allocation remains linear and contiguous. Each
process maps its own CPU address after importing a handle; a pointer received
through Binder is never reused directly. The HWC metadata path selects the
image FD, and legacy gralloc CPU lock/unlock performs real DMA-BUF cache sync.
The profile supports one plane and the RGB/R8 formats implemented by this
bridge; multi-plane YUV, FP16 and protected buffers are not implemented.

Applications need the KMS primary node for this in-process mapper, because
Utgard exposes `/dev/mali` instead of a DRM render node. The bring-up ueventd
rules therefore allow access to both `/dev/mali` and `/dev/dri/card0`.
The independent native checks completed shader compilation, drawing/readback,
KMS DMA-BUF import and native fence completion. After the devinfo input was
restored and the kernel fence ordering/lifetime defects were corrected,
Android's boot animation, lock screen, Launcher and Settings rendered with
`GLES: ARM, Mali-400 MP, OpenGL ES 2.0 58bf738` and advancing GP/PP interrupts.
The normal built-in-driver kernel (#8, `CONFIG_DEBUG_SPINLOCK=n`) reaches boot
completion on four CPUs and passes GPU draw/readback and Settings/Home
switching. An earlier boot's RCU stall was not conclusively diagnosed; longer
runtime, memory pressure and broader application compatibility remain tracked
in `TODO.md`. The GPU retains its fixed firmware clock. The temporary bring-up wake lock was removed after standby wakeup validation.

## Physical remote keys

The original board has AW9523 on I2C1/0x5b and AW9106 on I2C2/0x5b.
The stock kernel reads independent active-high inputs, rather than a matrix:
16 keys on AW9523 and two on AW9106 (OUT3/F8 and OUT4/F7).
The kernel uses its existing AW9523 driver, a six-line AW9106 GPIO driver,
and the common `gpio-keys` device named `a1260-remote` with 16 ms debounce.
I2C2 uses CLK_GATING_CTRL1 bit 16, not an adjacent bit to I2C0/I2C1.
Both expanders and the input device register automatically after reboot.

`keylayout/a1260-remote.kl` preserves the original `mtk-kpd.kl` mapping:
Home, Back, DPAD/OK, volume, channel and function keys retain their stock
identities. The owner confirmed the physical response and positions;
captured key events include balanced presses/releases from both expanders.
F-key reporting does not supply the original IR service/app actions.
Unrequested AW9106 LED channels are not configured by the GPIO driver.

## FT6336U touch

The reference BSP touch implementation and board parameters were confirmed
correct by the device owner. The kernel profile now enables the MT6580 Paris
pinctrl/EINT support, I2C1 with manual WRRD combination, MT6350 VGP1 regulation
and the existing EDT/FocalTech driver for FT6336U. VGP1 is enabled at 2.8 V;
reset is GPIO2, interrupt GPIO5/EINT5, and I2C1 uses address 0x38 with
GPIO59/SCL and GPIO62/SDA. No touch firmware is flashed.

On hardware, /dev/input/event0 reports INPUT_PROP_DIRECT, X=0..479, Y=0..799
and two contacts. Android InputReader identifies an internal TOUCH/TOUCH_MT
device. The input name EP0110M09 comes from upstream's firmware vendor-ID
heuristic; it is not an independent EDT panel identification. Real taps,
drags and releases generate interrupts and complete input events. The user
confirms accurate tapping and swiping; two-finger operation is unverified. This repair only
requires the rebuilt boot image; the existing Android input stack handles it.

A transient physical display misalignment was reported during this boot and
recovered without changes. The captured screenshot is normal, with zero
RDMA underflows and abnormal events; the cause remains unconfirmed.
The temporary suspend-prevention wake lock has been removed; the touch IRQ is disabled before hibernation so Android can enter standby without a spurious wake event.

## Normal userdebug ADB startup

The temporary `service.adb.root=1` override was removed on 2026-10-27.
ADB starts as shell on userdebug builds; `adb root` remains available on
request. `ro.secure`, `ro.adb.secure` and authorization keys are unchanged.

To prepare an existing first-stage ramdisk while retaining its static fsck:

```sh
python3 device/aic/a1260/tools/prepare_ramdisk.py input-ramdisk ramdisk-fsck.img
```

The tool removes the temporary property from
`system/etc/ramdisk/build.prop`, preserves the existing cpio entries and
checker, and optionally installs a new static ARM checker with
`--e2fsck PATH`. Full builds retain `PRODUCT_PACKAGES += e2fsck_ramdisk`.
The standard standalone pack configuration uses `ramdisk-fsck.img`.

## Factory battery gauge and cell label

The installed battery label is 1850 mAh, nominal 3.7 V, 6.845 Wh. The board
now sets design charge/energy separately from the unchanged 4.2 V charge
limit. Factory OAM D2 and the four original 102-point OCV/resistance curves
were recovered from the working device boot backup, replacing the earlier
1397 mAh reference model. Factory 25 C effective Qmax is 1838 mAh, distinct
from nominal design capacity. Sleep uses the original fixed 4 mA estimate;
that number is not a measured standby current.

`CONFIG_NVMEM=y` and `CONFIG_RTC_NVMEM=y` preserve the original RTC SOC
through a one-byte fixed-layout cell. RTC updates serialize with alarm writes
and verify masked readback. Source provenance, algorithm/rounding, deliberate
integration differences and validation limits are documented in
`kernel/aic/mt6580/Documentation/arm/mediatek/mt6350-stock-fuel-gauge.rst`.
Current on-device validation status is tracked in TODO.md.

## MIPI reference-clock ownership

Build #127 connects the MIPI PHY to the recovered MT6580 TOP/AP two-stage
26 MHz reference route. Both gates close after the PHY powers down and
reopen before PLL setup; display off/on and automatic deep/cable wake were
verified on hardware. The default ADB policy remains ordinary userdebug.
See `kernel/aic/mt6580/Documentation/arm/mediatek/mt6580-mipi26m.rst` for
factory Evidence, current LK comparison and unresolved physical rail wiring.

## PWRAP link qualification

Build #132 supports MT6350 serial-link initialization before PMIC consumers,
with the real TOPRGU reset, CCF source/gates, bounded calibration, DIO,
cipher and CRC setup. Normal startup retains a working firmware link. A
qualification boot reconstructed the link from reset (window 0x0000ff,
center 3), preserved VPROC, and passed seven CPU OPPs and three deep resumes.
The ordinary image contains no force-init parameter. Bootstrap supplies/pins
remain dependent on earlier firmware; online recovery is described below.
See `kernel/aic/mt6580/Documentation/arm/mediatek/mt6580-pwrap-link.rst`.

Builds #156/#157 add bounded online recovery with the old MT6580 SPI108 error
IRQ, public regmap access exclusion and serialization against SPM sleep
voltage/command ownership. It preserves eight autonomous slots and saved
AP configuration, reconstructs only the serial protocol, compares readable
VPROC snapshots, and never retries a failed original command. Three live
reconstructions, a soft inspection, seven OPPs, two complete deep resumes,
GPU/concurrent RTC access, aligned direct eMMC I/O and four-CPU checks passed.
One concurrent RTC read was explicitly rejected during reconstruction and
later reads recovered. Actual physical bus/CRC/error-IRQ injection remains
unqualified. See the kernel PWRAP document and runtime summary for limits.

The same build retains the bounded USB enumeration guard through RESET and
renews it for an unconfigured SUSPEND. Five screen-off root/unroot switches
reconnected automatically over USB/IP, without a temporary wake lock or
stay-on setting. Default ADB is still ordinary userdebug shell; other host
bus-suspend/remote-wakeup and bulk-transfer qualification remain pending.

## CPU and bus DCM

Build #133 enables BSP CPU Mode 1 and infra/peripheral automatic slow/gate
controls through the clock driver. CPUFreq pauses ARMPLL automatic gating;
deep pauses the owned controls before the PLL/mux transition and restores
them in the finisher. Seven OPPs, SMP/hotplug, three deep resumes and
post-resume graphics/direct storage passed; no late DCM repair was needed.
The normal backup confirms MCUSYS default ON is a no-op. MCUSYS/L2 DCM
remains unmodified; EMI DCM is added separately below. Four online CPUs continue to use WFI/DCM;
electrical savings have not been measured. See `mt6580-dcm.rst`.

## CPU0 runtime dormant

Build #150 adds a separate BSP 688-word deep-idle PCM, matched against the
normal device kernel backup. It admits CPU0 only after the other CPUs,
display and GPU are off; hotplug disables the state before bringing a
secondary online. The initial profile retains L2, infra, DDR and PLLs and
bounds each PCM residency to 20 ms. The four-core default is unchanged.
There is no automatic hotplug or MT6580 multicore MCDI implementation.

The WFI-abort fix captures ACTLR with volatile assembly before clearing
SMP: Clang previously delayed the non-volatile read until the return path,
leaving coherency disabled after an interrupted entry. Cortex-A7 erratum
814220 and the BSP L1-flush/L2-clean policy are also applied. Temporary
persistent markers, redundant GIC restoration and the explicit qualification
sysfs write interface have been removed.

#148 passed 528 actual resets and 46 WFI aborts; cleaned #149 reached 438
resets/28 aborts. Final #150 passed 83 resets/five aborts with ordinary menu
governor and automatic state admission, then restored four-core coherence.
RAM hashes and timer deadlines passed. See `mt6580-cpuidle.rst` and
`out/a1260-runtime-pm/dormant` for evidence and electrical-measurement limits.

The same final image passed two complete system-deep cycles, with RAM,
DCM/PWRAP restoration, Mali rendering/readback, aligned direct eMMC I/O and
four-core/clock-migration checks afterward. At #150, product `boot.img`/`kernel` used
`boot-dormant.img`/`kernel-dormant`; ordinary ADB remains shell.

## EMI clock management and inherited DRAMC policy

Build #151 enables the BSP EMI automatic gate, debounce15 and /32 idle ratio.
TOPCKGEN owns a named EMI register resource; DDR PLL frequency and timings
stay unchanged. The existing DRAMC policy was already enabled by preloader
and remains firmware-owned, with read-only policy and resume diagnostics.
EMI is paused before AXI/MAINPLL sleep transitions and restored in the early
finisher; unresolved restoration cannot proceed to normal device resume.

Two full deep cycles passed with EMI pause/restore2/2, matching policy and
RAM hashes, no late repair, and unchanged DRAMC enables. Four workers passed
64 MiB/eight memory-pattern rounds, including a run concurrent with Mali;
graphics readback, direct eMMC I/O and four-core coherence passed afterward.
Normal-governor CPU0 dormant completed 1237 actual resets with no failures.
The calculated memory source remains 266499673 Hz. Current product images
use `boot-memory-dcm.img`/`kernel-memory-dcm`; default ADB remains shell.
See `mt6580-memory-dcm.rst` and `out/a1260-runtime-pm/memory-dcm/summary.json`.

Final cleanup also allowed an automatic 55.747988563-second deep sleep;
total suspend success/failure became3/0, EMI early pause/restore3/3 and
DRAMC policy checks3/failures0, still with no late EMI repair.

Host access note (2026-10-27): the current default port5037 ADB server runs
on Windows. Leave the device attached to Windows for this workflow; USB/IP
attachment into WSL transfers ownership away from that server. An empty
ADB list after host reattachment is not by itself evidence of a kernel
crash. Do not create another ADB server/port or request WSL attachment
without first checking the current server's owner.

The final standard boot/kernel are build #157, with masked/raw IRQ causes
retained together. Normal shell boot, two live high/low OPP resets and one
soft inspection were repeated on that final build. The preceding #156 also
passed 1218 actual CPU0 dormant restores with RAM/four-CPU recovery. Root
access was used on demand and restored to ordinary shell after testing.

Build #161 enables the BSP CG1 MCUSYS and CG3 BIU bus policy while retaining
L2 /2 and leaving SRAM CG2 unchanged. CPUFreq and system deep serialize
pause/restore with their existing clock transactions. Hardware showed BIU
resets with CPU0 runtime power-down; it now restores in the PCM wait callback
before arch/GIC clients exit CPU PM, including abort cleanup. Seven OPPs,
full deep twice,911 runtime reset/restores with912 matched clock phases,
RAM/GPU/direct-I/O and four-core checks passed. Late repairs and clock
errors were zero; default Windows ADB reconnected automatically. Standard
boot/kernel now use #161, ordinary shell and normal power policy retained.
Physical power/current and failure injection are still unmeasured. See
`kernel/aic/mt6580/Documentation/arm/mediatek/mt6580-mcusys-dcm.rst`.

Build #165 adds the HACC software-key AES ECB/CBC provider, with queued
requests, bounded polling, key/data clearing and runtime/system PM. Full
crypto selftests on #164 and14400 concurrent comparison pairs across two
deep intervals on #165 passed, followed by1203 CPU0 idle restores and
RAM/GPU/direct-I/O checks. PIO measured slower than generic software AES;
priority50 keeps software preferred. Hardware-bound keys and TRNG are not
exposed. Standard boot/kernel are #165; temporary tests are removed and
ordinary ADB shell/four cores/default deep retained. See
`kernel/aic/mt6580/Documentation/arm/mediatek/mt6580-hacc.rst` for evidence,
the separately recorded freeze cancellation and remaining limits.

Build #168 adds display M4U/SMI with real IOVA scanout through RDMA port1.
COMMON/LARB0 own their BSP context and clock gates; MMSYS releases its LK
bootstrap after consumers synchronize. Real400-page walks include physically
scattered buffers; normal Android picture, allocation/CPU mmap/dma-buf handle
checks, display cycles, two deep sleeps and CPU0 idle/GPU/I/O recovery passed
on #166. Final #167 adds port bounds and repeats boot/buffer/scanout/power
checks. Final #168 uses cached diagnostics during teardown; standard boot/kernel
are #168 with ordinary shell/default CPU/deep
policy retained. LARB1 and hardware video decoding are still pending. See
`kernel/aic/mt6580/Documentation/arm/mediatek/mt6580-m4u.rst` for qualification
scope and retained script failure.
