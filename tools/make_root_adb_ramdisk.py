#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Select root adbd at initial startup in the A1260 bring-up ramdisk.

Android 17 first-stage init copies system/etc/ramdisk/build.prop into
second_stage_resources before mounting system. Boot-property loading preserves
service.adb.root from there; userdebug adbd then need not restart to become root.
No ro.secure, ro.adb.secure or authorization-key setting is changed.
"""
import argparse
import gzip
import stat
from pathlib import Path


def make_ramdisk(src: bytes) -> bytes:
    compressed = src.startswith(b"\x1f\x8b")
    data = gzip.decompress(src) if compressed else src
    target = "system/etc/ramdisk/build.prop"
    entries = []
    modes = {}
    prop = b""
    offset = 0
    next_ino = 1
    while True:
        if data[offset:offset + 6] not in (b"070701", b"070702"):
            raise ValueError(f"Invalid newc header at {offset}")
        fields = [int(data[i:i + 8], 16)
                  for i in range(offset + 6, offset + 110, 8)]
        size, name_size = fields[6], fields[11]
        name_end = offset + 110 + name_size
        if not name_size or name_end > len(data) or data[name_end - 1] != 0:
            raise ValueError("Invalid newc filename")
        name = data[offset + 110:name_end - 1].decode()
        body = (name_end + 3) & ~3
        end = (body + size + 3) & ~3
        if end > len(data):
            raise ValueError(f"Truncated newc entry: {name}")
        if name == "TRAILER!!!":
            break
        name = name.removeprefix("./")
        modes[name] = fields[1]
        next_ino = max(next_ino, fields[0] + 1)
        if name == target:
            if not stat.S_ISREG(fields[1]):
                raise ValueError("Ramdisk build.prop must be a regular file")
            prop = data[body:body + size]
        else:
            entries.append(data[offset:end])
        offset = end

    def add(name: str, mode: int, body: bytes = b"") -> None:
        nonlocal next_ino
        encoded = name.encode() + b"\0"
        fields = [next_ino, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1,
                  0, len(body), 0, 0, 0, 0, len(encoded), 0]
        next_ino += 1
        entry = b"070701" + b"".join(f"{v:08x}".encode() for v in fields)
        entry += encoded
        entry += b"\0" * (-len(entry) % 4)
        entry += body
        entry += b"\0" * (-len(entry) % 4)
        entries.append(entry)

    for directory in ("system", "system/etc", "system/etc/ramdisk"):
        if directory in modes:
            if not stat.S_ISDIR(modes[directory]):
                raise ValueError(f"Not a directory in ramdisk: {directory}")
        else:
            add(directory, stat.S_IFDIR | 0o755)
    lines = [line for line in prop.splitlines()
             if line.partition(b"=")[0].strip() != b"service.adb.root"]
    lines += [b"# Temporary A1260 bring-up: avoid a USB reset for adb root.",
              b"service.adb.root=1"]
    add(target, stat.S_IFREG | 0o644, b"\n".join(lines) + b"\n")
    add("TRAILER!!!", 0)
    result = b"".join(entries)
    result += b"\0" * (-len(result) % 512)
    return gzip.compress(result, mtime=0) if compressed else result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("Keep the original ramdisk: input and output must differ")
    args.output.write_bytes(make_ramdisk(args.input.read_bytes()))
    print(f"Wrote debug ramdisk: {args.output}")
