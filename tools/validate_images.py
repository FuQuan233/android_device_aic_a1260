#!/usr/bin/env python3
"""Check A1260 boot/system/vendor artifacts against its fixed GPT layout."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from validate_boot import require, validate as validate_boot


def validate_partition(path, filesystem, partition_bytes, source):
    with path.open('rb') as stream:
        prefix = stream.read(4096)
    stored_bytes = path.stat().st_size
    sparse = prefix[:4] == struct.pack('<I', 0xed26ff3a)
    logical_bytes = stored_bytes
    if sparse:
        # Use Android's sparse reader without expanding an image on disk.
        sys.path.insert(0, str(source / 'build/make/tools/releasetools'))
        from sparse_img import SparseImage
        image = SparseImage(str(path))
        try:
            require(image.blocksize == 4096, 'Expected 4096-byte sparse blocks')
            require(image.simg_f.tell() == stored_bytes,
                    f'{path.name}: truncated sparse data or unexpected trailing bytes')
            logical_bytes = image.blocksize * image.total_blocks
            prefix = b''.join(image.ReadBlocks(0, 1))
        finally:
            image.simg_f.close()
    require(4096 <= logical_bytes <= partition_bytes,
            f'{path.name}: expanded size {logical_bytes} exceeds partition '
            f'or is too small (limit {partition_bytes})')
    if filesystem == 'erofs':
        require(prefix[1024:1028] == struct.pack('<I', 0xe0f5e1e2),
                f'{path.name}: expected EROFS superblock')
        bits = prefix[1036]
        require(9 <= bits <= 16, 'Invalid EROFS block size')
        blocks = struct.unpack_from('<I', prefix, 1060)[0]
        if struct.unpack_from('<I', prefix, 1104)[0] & 0x80:
            blocks |= struct.unpack_from('<H', prefix, 1038)[0] << 32
        filesystem_bytes = blocks << bits
    elif filesystem == 'ext4':
        require(prefix[1080:1082] == b'\x53\xef',
                f'{path.name}: expected ext4 superblock')
        bits = struct.unpack_from('<I', prefix, 1048)[0]
        require(bits <= 6, 'Invalid ext4 block size')
        blocks = struct.unpack_from('<I', prefix, 1028)[0]
        if struct.unpack_from('<I', prefix, 1120)[0] & 0x80:
            blocks |= struct.unpack_from('<I', prefix, 1360)[0] << 32
        filesystem_bytes = blocks << (10 + bits)
    else:
        raise ValueError('Unsupported filesystem: ' + filesystem)
    require(0 < filesystem_bytes <= logical_bytes,
            f'{path.name}: filesystem extends beyond image payload')
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(data)
    return {'file': str(path), 'filesystem': filesystem, 'sparse': sparse,
            'stored_bytes': stored_bytes, 'expanded_bytes': logical_bytes,
            'partition_bytes': partition_bytes, 'sha256': digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product_out', type=Path)
    parser.add_argument('--source-root', type=Path,
                        default=Path(__file__).resolve().parents[4])
    args = parser.parse_args()
    try:
        report = {'boot': validate_boot(args.product_out / 'boot.img',
                                       args.source_root / 'kernel/aic/mt6580')}
        for name, fs, size in [('system', 'erofs', 1610612736),
                               ('vendor', 'ext4', 427819008)]:
            report[name] = validate_partition(args.product_out / (name + '.img'),
                                              fs, size, args.source_root)
        print(json.dumps(report, indent=2))
    except (OSError, ValueError, struct.error, IndexError,
            subprocess.CalledProcessError) as exc:
        parser.exit(1, 'A1260 image validation failed: ' + str(exc) + '\n')


if __name__ == '__main__':
    main()
