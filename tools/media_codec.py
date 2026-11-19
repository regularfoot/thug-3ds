"""Bounded real Bink/WAV decoder dependency; no transcoding or invented audio."""
import json
import hashlib
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PIN = 'db69d06eeeab4f46da15030a80d539efb4503ca8'  # official FFmpeg n7.1.1
SOURCE = ROOT / 'private/ffmpeg'
VIEW = ROOT / 'private/ffmpeg-native'
# Newlib defines uint32_t as unsigned long on this ARM ABI. Preserve that
# real type at the WAV tag address instead of suppressing pointer diagnostics.
PATCHES = {'libavformat/wavdec.c': [('unsigned int tag;', 'uint32_t tag;', 1)]}

# The 640x480 Bink streams need a single 3 MiB zeroed workspace. On the 3DS
# native heap the frame buffers and movie index can split that free space.
# Let the port lend a buffer from the game arena for this decoder's lifetime.
PATCHES['libavcodec/bink.c'] = [
    ('Bundle         bundle[BINKB_NB_SRC];',
     'AVBufferRef *port_workspace;\n    Bundle         bundle[BINKB_NB_SRC];', 1),
    ('    tmp = av_calloc(blocks, 64 * BINKB_NB_SRC);',
     """#if defined(__arm__) && defined(__GNUC__)
    extern AVBufferRef *rethpsu_bink_workspace(size_t) __attribute__((weak));
    if (rethpsu_bink_workspace)
        c->port_workspace = rethpsu_bink_workspace((size_t)blocks * 64 * BINKB_NB_SRC);
#endif
    tmp = c->port_workspace ? c->port_workspace->data : av_calloc(blocks, 64 * BINKB_NB_SRC);""", 1),
    ('    av_freep(&c->bundle[0].data);',
     '    if (c->port_workspace) av_buffer_unref(&c->port_workspace);\n    else av_freep(&c->bundle[0].data);', 1),
]

def prepare():
    validate()
    files = subprocess.check_output(['git', '-C', SOURCE, 'ls-files', '-z']).decode().split('\0')
    overrides = {}
    for name, changes in PATCHES.items():
        data = (SOURCE / name).read_bytes()
        for before, after, count in changes:
            if data.count(before.encode()) != count:
                raise AssertionError('Media portability input changed: ' + name)
            data = data.replace(before.encode(), after.encode())
        overrides[name] = data
    for name in files:
        if not name: continue
        destination = VIEW / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if name in overrides:
            if destination.is_symlink(): destination.unlink()
            data = overrides[name]
            if not destination.exists() or destination.read_bytes() != data: destination.write_bytes(data)
        elif destination.is_symlink():
            expected=os.path.relpath(SOURCE / name,destination.parent)
            if os.readlink(destination)!=expected:
                destination.unlink();destination.symlink_to(expected)
        else:
            if destination.exists(): raise AssertionError('Unexpected media compiler-view file: ' + name)
            destination.symlink_to(os.path.relpath(SOURCE / name, destination.parent))
    metadata=VIEW / '.git'
    if not metadata.is_symlink():
        if metadata.exists():raise AssertionError('Unexpected media compiler-view Git metadata')
        metadata.symlink_to('../ffmpeg/.git',target_is_directory=True)
    return hashlib.sha256(json.dumps(PATCHES, sort_keys=True).encode()).hexdigest()

def compiler_identity():
    validate()
    if not (VIEW / '.git').is_symlink() or os.readlink(VIEW / '.git')!='../ffmpeg/.git':
        raise AssertionError('Decoder compiler view lost pinned version metadata')
    hashes={}
    for name,changes in PATCHES.items():
        data=(SOURCE / name).read_bytes()
        for before,after,count in changes:
            if data.count(before.encode())!=count:raise AssertionError('Media source patch count changed')
            data=data.replace(before.encode(),after.encode())
        if (VIEW / name).is_symlink() or (VIEW / name).read_bytes()!=data:raise AssertionError('Media compiler view changed: '+name)
        hashes[name]=hashlib.sha256(data).hexdigest()
    for name in subprocess.check_output(['git','-C',SOURCE,'ls-files','-z']).decode().split('\0'):
        if not name or name in PATCHES:continue
        link=VIEW / name
        if not link.is_symlink() or os.readlink(link)!=os.path.relpath(SOURCE / name,link.parent):
            raise AssertionError('Media compiler-view source link changed: '+name)
    return hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()
COMMON = ['--disable-all', '--disable-everything', '--disable-autodetect',
          '--disable-doc', '--disable-programs', '--disable-avdevice', '--disable-avfilter',
          '--disable-swscale', '--disable-swresample', '--disable-network',
          '--disable-pthreads', '--disable-w32threads', '--disable-os2threads',
          '--disable-iconv', '--disable-zlib', '--disable-bzlib', '--disable-lzma',
          '--disable-sdl2', '--disable-x86asm', '--disable-asm', '--disable-runtime-cpudetect',
          '--enable-avformat', '--enable-avcodec', '--enable-avutil',
          '--enable-decoder=bink,binkaudio_dct,binkaudio_rdft,adpcm_ima_wav,pcm_s16le',
          '--enable-demuxer=bink,wav', '--enable-protocol=file',
          '--disable-shared', '--enable-static', '--enable-small']

def validate():
    if not (SOURCE / '.git').exists():
        raise AssertionError('Missing decoder source; run python3 -B tools/media_codec.py fetch')
    commit = subprocess.check_output(['git', '-C', SOURCE, 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', SOURCE, 'status', '--porcelain', '--untracked-files=no'], text=True)
    if commit != PIN or dirty:
        raise AssertionError('Decoder source must match the clean official pinned FFmpeg commit')
    return commit

def build_source_link(build, target):
    # FFmpeg configure intentionally reuses an existing src/configure before
    # consulting argv[0]. Switch only our generated symlink, never user source.
    link=build / 'src'
    if link.is_symlink():
        if os.readlink(link)==str(target): return False
        link.unlink()
    elif link.exists():
        raise AssertionError('Decoder build/src must be an owned symlink')
    link.symlink_to(target)
    return True

def build_host(execute, reports, environment):
    validate()
    build = ROOT / 'build/media-host'; build.mkdir(parents=True, exist_ok=True)
    flags = [*COMMON, '--cc=clang', '--extra-cflags=-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer',
             '--extra-ldflags=-fsanitize=address,undefined']
    stamp = build / 'source-flags.json'
    recipe = json.dumps([PIN, PATCHES, str(VIEW), *flags])
    source_changed=build_source_link(build,VIEW)
    if source_changed or not stamp.exists() or stamp.read_text() != recipe:
        execute([VIEW / 'configure', *flags], reports / 'media-host-configure.log', environment=environment, cwd=build)
        stamp.write_text(recipe)
    execute(['make', '-j3', 'libavformat/libavformat.a', 'libavcodec/libavcodec.a', 'libavutil/libavutil.a'],
            reports / 'media-host-build.log', environment=environment, cwd=build)

def arm_flags():
    return [*COMMON, '--arch=arm', '--cpu=arm1176jzf-s', '--target-os=none', '--enable-cross-compile',
            '--cc=/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc',
            '--ar=/opt/devkitpro/devkitARM/bin/arm-none-eabi-ar',
            '--ranlib=/opt/devkitpro/devkitARM/bin/arm-none-eabi-ranlib',
            '--nm=/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm',
            '--extra-cflags=-O2 -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -fno-short-enums -ffunction-sections -fdata-sections',
            '--extra-ldflags=-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -specs=3dsx.specs -L/opt/devkitpro/libctru/lib',
            '--extra-libs=-lctru -lm']

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=['fetch','prepare','verify'])
    args=parser.parse_args()
    if args.command=='fetch' and not SOURCE.exists():
        SOURCE.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run(['git','clone','--depth','1','--branch','n7.1.1','https://github.com/FFmpeg/FFmpeg.git',str(SOURCE)],check=True)
    validate()
    if args.command in ('fetch','prepare'):prepare()
    print('Verified official FFmpeg commit '+PIN+'; compiler view '+compiler_identity())
