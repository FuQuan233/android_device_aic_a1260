#!/usr/bin/env python3
"""Package the owner's Android 8.1 codec DSOs into an isolated worker directory.

No blobs or efuses are downloaded. The real same-device mt6580-devinfo.bin
already packaged by prepare_stock_mali.py remains the DRVB validation input.
The kernel's VAL/control adapter is built from its checked-in C sources.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("system", type=Path, help="owner firmware system/ extraction")
p.add_argument("destination", type=Path, help="local vendor/aic/a1260-codec")
p.add_argument("--kernel", required=True, type=Path)
p.add_argument("--cc", required=True, help="Android ARM32 clang driver")
a = p.parse_args()
out = a.destination
blobs = out / 'proprietary'
blobs.mkdir(parents=True, exist_ok=True)
# The VAL layout and internal decoder-state offsets were recovered against
# exactly this owner-supplied BSP. Do not silently accept another binary ABI.
EXPECTED = {'libstdc++.so': '76f954d3e42365127d6466ed9e1e9c0d0db4ae0eef330aa0a19606115f903d61',
'libbacktrace.so': 'fd3425e0a7633c563c38d9271d80ad9857f49e37fe27bceff7631ad1636fcce6',
 'libbase.so': '2146b92d9949fd1a0af9531973a2029a48911ec788c6e9f4fed7d0e45b8709fe',
 'libbwc.so': '59a877d9c0151a199fd13e5bf3677c344b3598ac178105b95b071b5f988a17f4',
 'libc++.so': '8a0a234632bd65004bf85c35c5bbc47e02aede2b72d2031a1ce303917b8c9d0b',
 'libcutils.so': 'd993da2d7b4ed6c846ba479bc6658ee69e264d3bc412be091f76e2a1b4913146',
 'libh264dec_customize.so': 'c2ec04eb40fe56bc689721ccd811c4c2e150f6e93ab6ed76b13bae829c575098',
 'libh264dec_sd.ca7.so': 'a24c193c0f7198ef6093c49c4ac2405f3cbfd07dcb92fe4b95859e1a7cf0de32',
 'libh264dec_xa.ca7.so': 'f7b903e0ca90ce89c8efbff5975daf3d2bde25ebcf42d2a0ee11fa54be733f2f',
 'libh264dec_xb.ca7.so': '3e1710a21a810b69afbcb75fb432cc3bdbd715d160bf5d34a209bde097be687e',
 'libion.so': '10fda6e635f034680b2754ef11abb2544bc4cf4868bb8ce8e6e2f6ed1a30a3a3',
 'libion_mtk.so': '4fb4bd358164f40754f7083049bc61114c7ae3acf021efead39b5eda84395b0c',
 'libion_ulit.so': '5e770943dcab63a16b5a5dd5da5bd089b30aa75130f6cce140857fbb8bfb38fe',
 'liblog.so': 'ded6624159a9725245ba465371ef9cf3d380e22e034abdb046ca81b67748fdc2',
 'liblzma.so': '24f6e13c35b02edd20c043722bdde1c4185ee78b274ac16a4513dad6baae07ed',
 'libm4u.so': 'c67feb737e9be915b28ac618e00307e2cbb1ec90e137ee82437ac512c69d9ac6',
 'libmmprofile.so': 'b274d1f122078ec5d09479316302748e6509019312511d156baaa594c6cabb25',
 'libmtk_drvb.so': '3a9dda4eda9991dd4d2a8a967caadaa854690ab57ebfb324647c6f363dd71436',
 'libunwind.so': '4c20db38138277612fbc877dc147bb9e2893e6c9f4fd08fcb2ebc4c47e258b49',
 'libutils.so': '9c0ba4ed1e235b5d378878d2500c4d2ccd4a81b682ff6a847ace586ade15e000',
 'libvcodec_oal.so': '59a16508e1bbd0672a688be2725feb19b5aeccaa4ae5bedb13b6d7faf681d197',
 'libvcodec_utility.so': '76e57462975c6c9753f577ad5625b87b73c20a69bfc4b29aeba1686946ab7e03',
 'libvcodecdrv.so': 'bcd9f53e4357368e626acb47f1157d03d62db89bbb6661df860ad0c36bf579a7'}
manifest = []
vendor = '''libvcodecdrv.so libvcodec_utility.so libvcodec_oal.so libmtk_drvb.so
libbwc.so libm4u.so libion_mtk.so libion_ulit.so libmmprofile.so
libh264dec_xa.ca7.so libh264dec_xb.ca7.so libh264dec_sd.ca7.so libh264dec_customize.so'''.split()
runtime = '''libutils.so libc++.so libbacktrace.so libbase.so libunwind.so
liblzma.so libcutils.so libion.so liblog.so libstdc++.so'''.split()
for directory, names in [(a.system / 'vendor/lib', vendor), (a.system / 'lib', runtime)]:
    for name in names:
        src = directory / name
        data = src.read_bytes()
        if hashlib.sha256(data).hexdigest() != EXPECTED[name]:
            p.error(f'unsupported stock ABI: {name}; qualify that firmware separately')
        if name == 'libmtk_drvb.so':
            old = b'/sys/bus/platform/drivers/dev_info/dev_info\0'
            new = b'/vendor/etc/mt6580-devinfo.bin\0'
            if data.count(old) != 1:
                p.error('unsupported DRVB: expected one original dev_info pathname')
            data = data.replace(old, new.ljust(len(old), b'\0'))
        (blobs / name).write_bytes(data)
        manifest.append({'file': name, 'source': str(src),
            'source_sha256': hashlib.sha256(src.read_bytes()).hexdigest(),
            'installed_sha256': hashlib.sha256(data).hexdigest()})
media = Path(__file__).resolve().parent.parent / 'media'
bridge = a.kernel / 'tools/mt6580-vcodec'
for name, sources in [
    ('libval-decode-bridge.so', [bridge / 'val-buffer-bridge.c', bridge / 'legacy-control-bridge.c']),
    ('libvndksupport.so', [media / 'vcodec_vndksupport.c']),
    ('libpowerhalwrap.so', [media / 'vcodec_power_hints.c']),
]:
    subprocess.run([a.cc, '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
        *map(str, sources), '-ldl', '-Wl,-soname,' + name, '-o', str(blobs / name)], check=True)
    manifest.append({'file': name, 'sources': [str(s) for s in sources],
        'source_sha256': [hashlib.sha256(s.read_bytes()).hexdigest() for s in sources],
        'installed_sha256': hashlib.sha256((blobs / name).read_bytes()).hexdigest()})
bp = ['// Generated locally from owner-supplied firmware; no blob redistribution.',
      'soong_namespace {}', '']
modules = []
for entry in manifest:
    name = entry['file']
    module = 'a1260-codec-' + name[:-3]
    modules.append(module)
    bp += ['cc_prebuilt_library_shared {', f'    name: "{module}",',
        f'    stem: "{name[:-3]}",', f'    srcs: ["proprietary/{name}"],',
        '    vendor: true,', '    compile_multilib: "32",',
        '    relative_install_path: "mt6580-codec",', '    strip: { none: true },',
        '    check_elf_files: false,', '}', '']
(out / 'Android.bp').write_text('\n'.join(bp))
(out / 'vcodec-vendor.mk').write_text(
    '# Generated local isolated stock codec package list.\n'
    'PRODUCT_SOONG_NAMESPACES += vendor/aic/a1260-codec\n'
    'PRODUCT_PACKAGES += \\\n    ' + ' \\\n    '.join(modules) + '\n')
(out / 'vcodec-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(f'Packaged {len(modules)} private codec DSOs in {out}')
