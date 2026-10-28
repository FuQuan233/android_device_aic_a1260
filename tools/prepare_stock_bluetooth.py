#!/usr/bin/env python3
"""Package only this unit's original BT RF record; no proprietary BT stack."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('nvram', type=Path, help='same-device BT_Addr record (64 or 66 bytes)')
parser.add_argument('output', type=Path, help='private vendor/aic/a1260-bluetooth directory')
args = parser.parse_args()
original = args.nvram.read_bytes()
if len(original) not in (64, 66):
    parser.error('BT_Addr must contain the 64-byte payload, optionally with its 2-byte trailer')
payload = original[:64]
if not any(payload[6:]):
    parser.error('BT_Addr has no calibration parameters')
args.output.mkdir(parents=True, exist_ok=True)
(args.output / 'mt6580-bt-calibration.bin').write_bytes(payload)
(args.output / 'bluetooth-vendor.mk').write_text(
    '# Same-device private RF calibration; never commit the payload.\n'
    'PRODUCT_COPY_FILES += \\\n'
    '    vendor/aic/a1260-bluetooth/mt6580-bt-calibration.bin:'
    '$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth/mt6580-bt-calibration.bin\n')
(args.output / 'provenance.json').write_text(json.dumps({
    'original_sha256': hashlib.sha256(original).hexdigest(),
    'payload_sha256': hashlib.sha256(payload).hexdigest(),
    'record_bytes': len(original),
    'payload_bytes': len(payload),
    'address_source': 'proinfo+0x68 via early read-only identity handover',
}, indent=2) + '\n')
