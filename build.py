"""Build the ARM application or the native data preparation tools."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from tools import media_codec

ROOT = Path(__file__).resolve().parent
TOOLS = ['source_bake_world_lods','source_bake_texture_cache','source_dump_world_mips','source_prepare_cas_resources']

def run(args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)

def ffmpeg(build, flags, jobs):
    build.mkdir(parents=True,exist_ok=True)
    media_codec.build_source_link(build, media_codec.VIEW)
    run([media_codec.VIEW/'configure', *flags],cwd=build)
    run(['make','-j',str(jobs),'libavformat/libavformat.a','libavcodec/libavcodec.a','libavutil/libavutil.a'],cwd=build)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target',choices=['arm','host'])
    parser.add_argument('-j','--jobs',type=int,default=4)
    args=parser.parse_args()
    if args.jobs < 1:parser.error('jobs must be positive')
    if not (ROOT/'private/original-build-sources.cmake').is_file():
        parser.error('First run python3 tools/prepare_engine.py /path/to/authorized/source')
    media_codec.compiler_identity()
    common=['cmake','-S',str(ROOT),'-G','Ninja','-DCMAKE_BUILD_TYPE=Debug','-DTHUG_SOURCE_SANITIZERS=OFF']
    import shutil
    if shutil.which('ccache'):common += ['-DCMAKE_C_COMPILER_LAUNCHER=ccache','-DCMAKE_CXX_COMPILER_LAUNCHER=ccache']
    if args.target=='arm':
        build=ROOT/'build/arm'; sdk=Path(os.environ.get('DEVKITPRO','/opt/devkitpro'))
        ffmpeg(build/'media',media_codec.arm_flags(),args.jobs)
        run([*common,'-B',str(build),'-DCMAKE_TOOLCHAIN_FILE='+str(sdk/'cmake/3DS.cmake')])
        run(['cmake','--build',build,'--parallel',str(args.jobs),'--target','thug1_full_engine_3dsx'])
    else:
        build=ROOT/'build/host'
        ffmpeg(ROOT/'build/media-host', [*media_codec.COMMON,'--cc=clang'],args.jobs)
        run([*common,'-B',str(build),'-DCMAKE_C_COMPILER=clang','-DCMAKE_CXX_COMPILER=clang++'])
        run(['cmake','--build',build,'--parallel',str(args.jobs),'--target',*TOOLS])

if __name__=='__main__':
    main()
