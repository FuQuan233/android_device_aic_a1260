#!/usr/bin/env python3
"""Package this unit's original CONSYS firmware and private calibration.

The generated vendor tree is local only. Never commit the owner's binaries,
factory RF calibration or device identifiers to the device/kernel repository.
"""
import argparse
import hashlib
import json
from pathlib import Path


EXPECTED = {
    'firmware/WIFI_RAM_CODE_6580': '0aca2c5a843af3001dfffedf7edcc251bc4d3c39879987bc85f3a1ba95b664d5',
    'firmware/ROMv2_lm_patch_1_0_hdr.bin': 'f1a025e2ce70430c2baae6a27c9c8dc5946614c544c96d5491c7f6234bf81817',
    'firmware/ROMv2_lm_patch_1_1_hdr.bin': '098dbe0b690d1345436dca54860798cd4c49dbea4d0ed1732d07587bea558fa8',
    'firmware/WMT_SOC.cfg': 'cea0a94b8a88e9c24e6835446ebc3106d7b100c3709ea98e8bb7368689c8a2b5',
}


def write_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('system', type=Path, help="owner firmware's system/ extraction")
    parser.add_argument('destination', type=Path, help='local vendor/aic/a1260-wifi')
    parser.add_argument('--nvram', type=Path, required=True,
                        help='same-device WIFI record: 512-byte payload or original 514-byte file')
    args = parser.parse_args()
    payload = args.nvram.read_bytes()
    if len(payload) not in (512, 514):
        parser.error('WIFI calibration must be the same-device 512/514-byte record')
    calibration = payload[:512]
    if calibration[:2] in (b'\x00\x00', b'\xff\xff'):
        parser.error('invalid factory calibration version')
    manifest = []
    for relative, expected in EXPECTED.items():
        source = args.system / 'vendor' / relative
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            parser.error('unqualified owner firmware ABI: ' + relative)
        write_changed(args.destination / 'proprietary' / relative, data)
        manifest.append({'file': relative, 'source_sha256': expected})
    name = 'firmware/mt6580-wifi-nvram.bin'
    write_changed(args.destination / 'proprietary' / name, calibration)
    manifest.append({'file': name, 'bytes': 512,
                     'source_bytes': len(payload),
                     'source_sha256': hashlib.sha256(payload).hexdigest(),
                     'payload_sha256': hashlib.sha256(calibration).hexdigest()})
    bp = ['// Firmware/calibration copy rules only; dispatcher is built from source.',
          'soong_namespace {}', '']
    mk = ['# Local original firmware; calibration is private to this unit.',
          'PRODUCT_COPY_FILES += ' + chr(92)]
    firmware = [m['file'] for m in manifest if m['file'].startswith('firmware/')]
    for i, relative in enumerate(firmware):
        continuation = ' ' + chr(92) if i + 1 < len(firmware) else ''
        mk.append('    vendor/aic/a1260-wifi/proprietary/' + relative +
                  ':$(TARGET_COPY_OUT_VENDOR)/' + relative + continuation)
    write_changed(args.destination / 'Android.bp', ('\n'.join(bp) + '\n').encode())
    write_changed(args.destination / 'wifi-vendor.mk', ('\n'.join(mk) + '\n').encode())
    write_changed(args.destination / 'wifi-manifest.json',
                  (json.dumps(manifest, indent=2) + '\n').encode())
    print('Packaged original firmware and same-device calibration locally')


if __name__ == '__main__':
    main()
