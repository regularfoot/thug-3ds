"""Create a new personal SD directory from a supported PC installation and a local ARM build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import struct
import zlib
import sys
import tempfile
from tools.pack_npc_resources import pack
from tools.bake_scenery_effects import bake as bake_scenery_effects

ROOT = Path(__file__).resolve().parent
BUILD_ID_HEADER = ROOT / 'engine/platform/source_port/build_id.h'
build_id = re.search(r'^#define THUG_SOURCE_BUILD "([0-9]{4})"$',
                     BUILD_ID_HEADER.read_text(), re.MULTILINE)
if build_id is None:
    raise ValueError(f'Missing four-digit build number in {BUILD_ID_HEADER}')
APP = 'thug1_source_engine_' + build_id.group(1)

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()

def copy_retail(src, dst, profile):
    inputs={}
    for p in src.rglob('*'):
        rel=p.relative_to(src)
        if any(part.startswith('.') for part in rel.parts) or not p.is_file():
            continue
        name=rel.as_posix().lower()
        if name in inputs or p.is_symlink():raise ValueError(f'Unexpected duplicate or symbolic link: {rel}')
        inputs[name]=p
    if len(inputs)!=profile['retail_count']:
        raise ValueError('Unsupported or modified PC Data directory (file count differs)')
    fingerprint=hashlib.sha256()
    for name,p in sorted(inputs.items()):
        target=dst/name
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(p,target)
        fingerprint.update((name+'\0'+sha(target)+'\n').encode())
    if fingerprint.hexdigest()!=profile['retail_sha256']:
        raise ValueError('PC data does not match the reference edition. Use an unmodified installation.')

def compact_texture_cache(source, target):
    # Store the retail index immediately before its payload, in lookup order.
    # The baker reserves space for its maximum entry count while discovering textures.
    header = struct.Struct('<4I')
    entry = struct.Struct('<6I')
    def checksum(data):
        return zlib.crc32(data) ^ 0xffffffff
    with source.open('rb') as raw, target.open('xb') as out:
        version, limit, count, expected = header.unpack(raw.read(header.size))
        if version != 0x3143544e or not 0 < count <= 8192:
            raise ValueError('Invalid source texture cache')
        index = raw.read(count * entry.size)
        if len(index) != count * entry.size or checksum(index) != expected:
            raise ValueError('Invalid texture index')
        out.seek(header.size + len(index))
        packed = bytearray()
        for group, length, kind, offset, size, crc in entry.iter_unpack(index):
            raw.seek(offset)
            data = raw.read(size)
            if len(data) != size or checksum(data) != crc:
                raise ValueError('Damaged texture group')
            packed.extend(entry.pack(group, length, kind, out.tell(), size, crc))
            out.write(data)
        out.seek(0)
        out.write(header.pack(version, limit, count, checksum(packed)))
        out.write(packed)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',required=True,type=Path,help='Original PC Game/Data directory')
    parser.add_argument('--save-dir',type=Path,help='Optional existing source-saves directory')
    parser.add_argument('--autoload',help='Exact in-game name of an existing save')
    parser.add_argument('--sdk-image',help='Docker image containing tex3ds, if tex3ds is not on PATH')
    args=parser.parse_args()
    if args.autoload and (not args.save_dir or len(args.autoload.encode())>32 or any(c in args.autoload for c in '/\\\r\n')):
        parser.error('Provide a matching --save-dir and valid in-game save name')
    profile=json.loads((ROOT/'tools/data-profile.json').read_text())
    source=args.data.resolve(strict=True)
    target=ROOT/'out/3ds'/APP
    if target.exists():raise FileExistsError(f'{target} already exists. Move the previous result first.')
    if ROOT.is_relative_to(source):raise ValueError('Use a PC Data directory outside the build kit.')
    target.parent.mkdir(parents=True,exist_ok=True)
    arm=ROOT/'build/arm'
    host=ROOT/'build/host'
    def tool(name,*arguments):
        subprocess.run([host/name,*arguments],check=True)
    with tempfile.TemporaryDirectory(prefix='.stage-',dir=target.parent) as temp:
        stage=Path(temp); app=stage/'app'; data=app/'data'; data.mkdir(parents=True)
        for ext in ('3dsx','smdh'):
            shutil.copyfile(arm/('thug1_full_engine.'+ext),app/(APP+'.'+ext))
        print('Checking and copying the supported retail PC data',flush=True)
        copy_retail(source,data,profile)
        # Extract the missing custom-skater UI textures from the user's own PRE.
        tool('source_prepare_cas_resources',source,data)
        low=data/'pre/lowmem'
        tool('source_prepare_cas_resources','--disk-pre',data,low)
        defaults={'npcparts','sc2','sc2ped','sc2scn','skater_sounds','skaterparts','themes'}
        tool('source_prepare_cas_resources','--disk-pre',data,low,*sorted(set(profile['disk_archives'])-defaults))
        pack(data,low/'npcparts.pak')
        for flag in ['normal','opaque']:
            if profile['lods'][flag]:
                tool('source_bake_world_lods',data,data/'lods',*(['--opaque-surfaces'] if flag=='opaque' else []),*profile['lods'][flag])
        tool('source_bake_world_lods',data,data/'lods/old','--old-surfaces')
        bake_scenery_effects(data)
        cache = stage/'retail-cache.3tc'
        tool('source_bake_texture_cache',data,cache)
        compact_texture_cache(cache, data/'textures.3tc')
        tool('source_bake_texture_cache',data,data/'venice.3tc','--venice-low-memory')
        mips=stage/'mips'
        tool('source_dump_world_mips',data,mips,'128','nj','ny')
        baker=ROOT/'tools/bake_world_textures.py'
        if args.sdk_image:
            inside=lambda path: '/project/'+path.relative_to(ROOT).as_posix()
            subprocess.run(['docker','run','--rm','--platform','linux/amd64','--network=none',
                '-v',str(ROOT)+':/project',args.sdk_image,'python3',inside(baker),inside(mips),inside(data/'worldtex'),'nj','ny'],check=True)
        else:
            subprocess.run([sys.executable,baker,mips,data/'worldtex','nj','ny'],check=True)
        for relative in profile['required_generated']:
            if not (data/relative).is_file() or (data/relative).stat().st_size==0:
                raise RuntimeError('Missing generated resource: '+relative)
        if args.save_dir:
            shutil.copytree(args.save_dir.resolve(strict=True),app/'source-saves')
            if args.autoload:(app/'autoload_save.txt').write_text(args.autoload+'\n')
        # A fresh install plays all three original opening movies.
        if target.exists():raise FileExistsError(target)
        app.rename(target)
    print('Ready:',target)

if __name__=='__main__':
    main()
