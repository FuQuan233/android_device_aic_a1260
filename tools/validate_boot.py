#!/usr/bin/env python3
"""Validate the device's actual legacy boot artifact before hardware testing."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def archive_file(archive, name):
    result = subprocess.run(['cpio', '-i', '--quiet', '--to-stdout', name],
                            input=archive, capture_output=True, check=True)
    require(bool(result.stdout), 'Missing ramdisk file: ' + name)
    return result.stdout


def validate(boot, source):
    image = boot.read_bytes()
    require(image[:8] == b'ANDROID!', 'Missing legacy Android boot header')
    require(len(image) <= 16777216, 'boot.img exceeds the 16 MiB partition')
    fields = struct.unpack_from('<10I', image, 8)
    size, address, rd_size, rd_address, second_size, _, tags, page, version, _ = fields
    require(version == 0 and page == 2048, 'Expected header v0 with 2048-byte pages')
    require((address, rd_address, tags) == (0x80008000, 0x84000000, 0x8e000000),
            'Boot addresses differ from captured A1260 geometry')
    require(second_size == 0, 'Unexpected second-stage bootloader')
    kernel = image[page:page + size]
    rd_offset = page + (size + page - 1) // page * page
    ramdisk = image[rd_offset:rd_offset + rd_size]
    require(len(kernel) == size and len(ramdisk) == rd_size, 'Truncated boot payload')
    require(kernel[0x24:0x28] == b'\x18\x28\x6f\x01', 'Expected ARM zImage')
    start, end = struct.unpack_from('<II', kernel, 0x28)
    dtb = kernel[end - start:]
    require(dtb[:4] == b'\xd0\x0d\xfe\xed', 'Missing appended device DTB')
    require(struct.unpack_from('>I', dtb, 4)[0] == len(dtb), 'Invalid appended DTB size')
    require(ramdisk[:2] == b'\x1f\x8b', 'Expected gzip external ramdisk')
    archive = gzip.decompress(ramdisk)
    init = archive_file(archive, 'init')
    require(init[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<H', init, 18)[0] == 40,
            'First-stage init must be a little-endian ARM32 ELF')
    phoff = struct.unpack_from('<I', init, 28)[0]
    phsize, phcount = struct.unpack_from('<HH', init, 42)
    require(all(struct.unpack_from('<I', init, phoff + n * phsize)[0] != 3
                for n in range(phcount)), 'First-stage init must be statically linked')
    fstab = archive_file(archive, 'fstab.mt6580').decode()
    entries = {p[1]: p for line in fstab.splitlines()
               if (p := line.split()) and not p[0].startswith('#')}
    require(entries['/system'][0:3] == ['/dev/block/by-name/system', '/system', 'erofs'],
            'Wrong system partition or filesystem in first-stage fstab')
    require(entries['/vendor'][0:3] == ['/dev/block/by-name/vendor', '/vendor', 'ext4'],
            'Wrong vendor partition or filesystem in first-stage fstab')
    with tempfile.TemporaryDirectory(prefix='a1260-boot-') as temp:
        temp = Path(temp)
        (temp / 'kernel').write_bytes(kernel)
        (temp / 'board.dtb').write_bytes(dtb)
        compatible = subprocess.check_output(['fdtget', str(temp / 'board.dtb'), '/',
                                               'compatible'], text=True)
        require('wingtech,wt6580-we-n' in compatible, 'Wrong board DTB')
        config = subprocess.check_output([str(source / 'scripts/extract-ikconfig'),
                                           str(temp / 'kernel')], text=True)
    selected = set(config.splitlines())
    cmdline = next(line for line in selected if line.startswith('CONFIG_CMDLINE='))
    require('androidboot.hardware=mt6580' in cmdline,
            'Android hardware identifier must match init.mt6580.rc and fstab.mt6580')
    require('androidboot.boot_devices=11120000.mmc' in cmdline,
            'Boot device must enable Android by-name links for the eMMC controller')
    for setting in ('CONFIG_ARM_APPENDED_DTB_INITRD_FROM_BOOTLOADER=y',
                    'CONFIG_INITRAMFS_SOURCE=""', 'CONFIG_USB_CONFIGFS_F_FS=y',
                    'CONFIG_USB_CONFIGFS_ACM=y', '# CONFIG_USB_G_SERIAL is not set',
                    'CONFIG_VFP=y', 'CONFIG_NEON=y', 'CONFIG_SECURITY_SELINUX=y',
                    'CONFIG_DM_VERITY=y', 'CONFIG_CRYPTO_SHA256=y',
                    'CONFIG_CRYPTO_SHA512=y'):
        require(setting in selected, 'Missing built kernel setting: ' + setting)
    return {'product': 'lineage_a1260', 'boot_bytes': len(image),
            'boot_sha256': hashlib.sha256(image).hexdigest(), 'header_version': version,
            'external_init_arm32_static': True, 'fstab_checked': True,
            'bootloader_initrd_handover_enabled': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('boot', type=Path)
    parser.add_argument('--kernel-source', type=Path, default=Path('kernel/aic/mt6580'))
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.boot, args.kernel_source.resolve()), indent=2))
    except (ValueError, KeyError, struct.error, subprocess.CalledProcessError) as exc:
        parser.exit(1, 'A1260 boot validation failed: ' + str(exc) + '\n')


if __name__ == '__main__':
    main()
