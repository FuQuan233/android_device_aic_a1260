#!/usr/bin/env python3
"""Build ARMv7 Lima EGL/GLES against an NDK and this tree's platform libdrm.

Requires Python packages meson (>=1.4), mako, packaging and PyYAML.
Android stub DSOs are link-time interfaces only and must not be installed.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--toolchain", type=Path, required=True,
                        help="NDK LLVM prebuilt directory containing bin/ and sysroot/")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("-j", type=int, default=8)
    args = parser.parse_args()
    root, ndk, out = (p.resolve() for p in (args.android_root, args.toolchain, args.out))
    out.mkdir(parents=True, exist_ok=True)
    pc = out / "pkgconfig"
    pc.mkdir(exist_ok=True)
    version = re.search(r"version\s*:\s*'([^']+)'",
                        (root / "external/libdrm/meson.build").read_text()).group(1)
    (pc / "libdrm.pc").write_text(f"""Name: libdrm
Description: A1260 Android platform libdrm
Version: {version}
Libs: -L{root}/out/target/product/a1260/vendor/lib -ldrm
Cflags: -I{root}/external/libdrm -I{root}/external/libdrm/include/drm
""")
    cross = "[binaries]\n"
    for name, command in {
        "c": [str(ndk / "bin/clang"), "--target=armv7a-linux-androideabi35", "-march=armv7-a", "-mfpu=neon"],
        "cpp": [str(ndk / "bin/clang++"), "--target=armv7a-linux-androideabi35", "-march=armv7-a", "-mfpu=neon"],
        "ar": [str(ndk / "bin/llvm-ar")],
        "strip": [str(ndk / "bin/llvm-strip")],
        "pkg-config": ["/usr/bin/pkg-config"],
    }.items():
        cross += f"{name} = {command!r}\n"
    cross += f"""
[host_machine]
system = 'android'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '{pc}'
"""
    crossfile = out / "arm-android.ini"
    crossfile.write_text(cross)
    build = out / "build"
    env = dict(os.environ, NINJA=str(root / "prebuilts/build-tools/linux-x86/bin/ninja"))
    meson = [sys.executable, "-m", "mesonbuild.mesonmain"]
    setup = meson + ["setup", str(build), str(root / "external/mesa3d"),
                     "--cross-file", str(crossfile), "--buildtype", "release",
                     "--prefix", "/vendor", "--libdir", "lib"]
    if (build / "meson-private/coredata.dat").exists():
        setup.append("--reconfigure")
    options = {
        "platforms": "android", "android-stub": "true", "platform-sdk-version": "37",
        "gallium-drivers": "lima", "vulkan-drivers": "", "llvm": "disabled",
        "glx": "disabled", "gbm": "disabled", "gles1": "enabled", "gles2": "enabled",
        "egl": "enabled", "shared-glapi": "enabled", "android-libbacktrace": "disabled",
        "valgrind": "disabled", "libunwind": "disabled", "xmlconfig": "disabled",
        "zstd": "disabled", "zlib": "disabled", "shader-cache": "disabled",
        "egl-lib-suffix": "_mesa", "gles-lib-suffix": "_mesa", "gallium-va": "disabled",
    }
    subprocess.run(setup + [f"-D{k}={v}" for k, v in options.items()], env=env, check=True)
    if not args.configure_only:
        subprocess.run(meson + ["compile", "-C", str(build), "-j", str(args.j)], env=env, check=True)
        stage = out / "install"
        subprocess.run(meson + ["install", "-C", str(build), "--no-rebuild", "--strip",
                                 "--destdir", str(stage)], env=env, check=True)
        # Install removes build RPATHs. Never ship the Android link-time stub DSOs.
        dest = Path(__file__).resolve().parents[1] / "prebuilts/mesa/arm"
        dest.mkdir(parents=True, exist_ok=True)
        for name in ("libEGL_mesa.so", "libGLESv1_CM_mesa.so", "libGLESv2_mesa.so", "libgallium_dri.so"):
            shutil.copy2(stage / "vendor/lib" / name, dest / name)
        shutil.copy2(ndk / "sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so",
                     dest / "libc++_shared.so")
        shutil.copy2(root / "external/mesa3d/LICENSE", dest.parent / "LICENSE.mesa")


if __name__ == "__main__":
    main()
