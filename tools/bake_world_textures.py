"""Bake THUG1's exported opaque mip levels into PICA200 ETC1 banks.

Export inputs with source_dump_world_mips, then run this tool with devkitPro
tex3ds on PATH. Each original mip is encoded independently of its neighbours.
"""

import argparse
from collections import defaultdict
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


def encode_ppm(tex3ds, rgb, width, height, temp):
    source = temp / "input.ppm"
    output = temp / "output.bin"
    source.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)
    subprocess.run(
        [tex3ds, "-f", "etc1", "-q", "high", "-r", "-z", "none", "-o", output, source],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    raw = output.read_bytes()
    size = width * height // 2
    if len(raw) != 4 + size or int.from_bytes(raw[:4], "little") != size << 8:
        raise ValueError(f"Unexpected tex3ds raw header or size for {width}x{height}")
    return raw[4:]


def read_rgb(path, width, height):
    rgba = path.read_bytes()
    if len(rgba) != width * height * 4 or any(alpha != 255 for alpha in rgba[3::4]):
        raise ValueError(f"Expected an opaque original level in {path}")
    rgb = bytearray(width * height * 3)
    for channel in range(3):
        rgb[channel::3] = rgba[channel::4]
    return rgb


def encode_group(tex3ds, paths, width, height, temp):
    # Group equal sizes to keep tex3ds startup out of the inner loop.
    # An ETC tile never crosses the boundaries of these 8-aligned rectangles.
    columns = min(512 // width, 1 << (len(paths) - 1).bit_length())
    rows = (len(paths) + columns - 1) // columns
    atlas_width, atlas_height = width * columns, height * (1 << (rows - 1).bit_length())
    rgb = bytearray(atlas_width * atlas_height * 3)
    row_bytes = width * 3
    for index, path in enumerate(paths):
        tile = read_rgb(path, width, height)
        x, y = index % columns * width, index // columns * height
        for line in range(height):
            start = ((y + line) * atlas_width + x) * 3
            rgb[start:start + row_bytes] = tile[line * row_bytes:(line + 1) * row_bytes]
    encoded = encode_ppm(tex3ds, rgb, atlas_width, atlas_height, temp)
    tile_row_bytes = width * 4
    result = []
    for index in range(len(paths)):
        x, y = index % columns * width, index // columns * height
        rows = [((y + line) * atlas_width // 2 + x * 4) for line in range(0, height, 8)]
        result.append(b"".join(encoded[start:start + tile_row_bytes] for start in rows))

    # Independently encode a tile to validate both the byte order and extraction.
    sample = len(paths) - 1
    reference = encode_ppm(tex3ds, read_rgb(paths[sample], width, height), width, height, temp)
    if result[sample] != reference:
        raise ValueError(f"ETC1 atlas does not match the independent encoding for {paths[sample]}")
    return result


def bake(tex3ds, source, output, level, temp):
    with (source / f"{level}.txt").open() as manifest:
        dictionary_size, limit, dictionary_crc = map(int, next(manifest).split())
        entries = sorted(tuple(map(int, line.split())) for line in manifest)
    if not entries or len({entry[0] for entry in entries}) != len(entries):
        raise ValueError(f"Expected a nonempty texture list with unique IDs for {level}")

    groups = defaultdict(list)
    for key, width, height, levels in entries:
        for mip in range(levels):
            groups[width >> mip, height >> mip].append((key, mip))

    encoded = {}
    for (width, height), group in groups.items():
        per_atlas = (512 // width) * (512 // height)
        for start in range(0, len(group), per_atlas):
            batch = group[start:start + per_atlas]
            paths = [source / f"{level}-{key}-{mip}.rgba" for key, mip in batch]
            encoded.update(zip(batch, encode_group(tex3ds, paths, width, height, temp), strict=True))

    payload = bytearray()
    texture_bytes = 0
    for key, width, height, levels in entries:
        chain = b"".join(encoded[key, mip] for mip in range(levels))
        payload += struct.pack("<IHHII", key, width, height, levels, len(chain))
        payload += chain
        texture_bytes += len(chain)
    checksum = zlib.crc32(payload) ^ 0xFFFFFFFF
    header = struct.pack("<4sIIIII", b"ETC1", dictionary_size, limit, dictionary_crc, len(entries), checksum)
    if len(header) + len(payload) > 2 * 1024 * 1024:
        raise ValueError(f"{level}: ETC1 bank exceeds the runtime limit")
    (output / f"{level}.etc").write_bytes(header + payload)
    print(f"{level}: {len(entries)} textures, {len(encoded)} mips, {texture_bytes} ETC1 bytes vs "
          f"{texture_bytes * 4} RGB565 bytes, {len(header) + len(payload)} bank bytes", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("levels", nargs="+")
    parser.add_argument("--tex3ds", default="tex3ds")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="thug-etc1-") as temporary:
        for level in args.levels:
            bake(args.tex3ds, args.source, args.output, level, Path(temporary))


if __name__ == "__main__":
    main()
