"""Index named shadow and reflection geometry in the user's own level files."""
from pathlib import Path
import re
import struct
import zlib


def member(archive, key):
    with archive.open("rb") as source:
        magic, count = struct.unpack("<4sI", source.read(8))
        if magic != b"DPM1" or count > 65535:
            raise ValueError(f"Invalid level archive {archive}")
        for _ in range(count):
            name, offset, size = struct.unpack("<128sII", source.read(136))
            if name.partition(b"\0")[0].decode("ascii").lower() == key:
                source.seek(offset)
                data = source.read(size)
                if len(data) != size:
                    raise ValueError(f"Incomplete {key} in {archive}")
                return data


# Keep solid alternate meshes such as HI's sand and SE's runway. Their
# geometry carries the actual playing surface with baked-in light and dark.
def effect_type(name):
    if any(word in name for word in (b"noshadow", b"no_shadow", b"shadowon", b"shadowoff",
                                       b"shadow_on", b"shadow_off", b"sand_comp")):
        return 0
    if b"shadow" in name:
        return 1
    if b"reflect" in name and not any(word in name for word in (b"glass", b"window")):
        return 2
    return 0


def bake(data):
    archives = data / "pre/lowmem"
    target = data / "lods/old"
    target.mkdir(parents=True, exist_ok=True)
    for scene_archive in sorted(archives.glob("*scn.pak")):
        level = scene_archive.stem[:-3]
        archive = archives / (level + ".pak")
        if not archive.is_file():
            continue
        base = f"levels/{level}/{level}"
        qb = member(archive, base + ".qb")
        if not qb:
            continue
        names = {}
        for match in re.finditer(rb"\x2b(.{4})([A-Za-z0-9_ -]{1,160})\x00", qb, re.DOTALL):
            name = match[2].lower()
            # SJ uses the same named banks throughout the grandstand. The loader
            # also checks the chair assembly against its source materials.
            seat_bank = level == "sj" and re.fullmatch(rb"g_(?:seats[abc]|topseats)_[0-9]+", name)
            kind = 3 if seat_bank else effect_type(name)
            key = int.from_bytes(match[1], "little")
            if kind and key == (zlib.crc32(name) ^ 0xffffffff):
                names[key] = kind
        scene = member(scene_archive, base + ".scn") if names else None
        if scene:
            header = struct.pack("<4sIII", b"FX01", len(scene), zlib.crc32(scene) ^ 0xffffffff, len(names))
            records = b"".join(struct.pack("<IB", key, kind) for key, kind in sorted(names.items()))
            (target / (level + ".3fx")).write_bytes(header + records)
