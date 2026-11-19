#!/usr/bin/env python3
"""Pack loose retail pedestrian parts for the port's disk PRE reader."""
import argparse
import struct
from pathlib import Path


def pack(data_root, output):
    models = data_root / "models"
    directories = [path for path in models.iterdir()
                   if path.is_dir() and path.name.lower() in {"ped_male", "ped_female"}]
    entries = {}
    for directory in directories:
        for path in directory.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in {".cas", ".skin", ".tex", ".txx"}:
                continue
            name = path.relative_to(data_root).as_posix().lower().encode("ascii")
            if name in entries or len(name) >= 128:
                raise ValueError(f"Invalid disk PRE key {name!r}")
            entries[name] = path.read_bytes()
    if not 0 < len(entries) <= 2048:
        raise ValueError(f"Invalid pedestrian part count {len(entries)}")
    ordered = sorted(entries.items())
    offset = 8 + len(ordered) * 136
    index = bytearray(struct.pack("<4sI", b"DPM1", len(ordered)))
    for name, content in ordered:
        index.extend(struct.pack("<128sII", name, offset, len(content)))
        offset += len(content)
    with output.open("xb") as stream:
        stream.write(index)
        for _, content in ordered:
            stream.write(content)
    return len(ordered), offset


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    count, size = pack(args.data, args.output)
    print(f"Packed {count} original files in {size} bytes")
