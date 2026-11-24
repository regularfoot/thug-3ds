"""Bake the three PC opening movies into streaming PICA textures and stereo IMA WAV."""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

def bake(source, output, encoder):
    for name in ('atvi', 'nslogo', 'intro'):
        movie = source / (name + '.bik')
        command = ['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(movie)]
        with (output / (name + '.etv')).open('xb') as out:
            out.write(struct.pack('<4sHHII', b'THV1', 256, 128, 24, 1))
            out.flush()
            with subprocess.Popen(command + ['-map', '0:v:0', '-vf', 'fps=24,scale=256:128:flags=area',
                                            '-pix_fmt', 'rgba', '-f', 'rawvideo', '-'], stdout=subprocess.PIPE) as video:
                try:
                    subprocess.run([encoder], stdin=video.stdout, stdout=out, check=True)
                finally:
                    video.stdout.close()
                if video.wait():
                    raise RuntimeError(f'FFmpeg failed on {movie}')
            size = out.tell() - 16
            if not size or size % 16384:
                raise ValueError(f'Incomplete video for {name}')
        subprocess.run(command + ['-map', '0:a:0', '-ac', '2', '-ar', '32000', '-c:a',
                                  'adpcm_ima_wav', '-n', str(output / (name + '.wav'))], check=True)
        print(name, size // 16384, 'frames', flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bik_directory', type=Path)
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / 'engine'
    codec = root / 'third_party/rg_etc1'
    target = args.output_directory.resolve()
    if target.exists():
        raise FileExistsError(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent) as tmp:
        staging = Path(tmp) / 'movies'
        staging.mkdir()
        encoder = Path(tmp) / 'encode_etc'
        subprocess.run(['c++', '-std=c++17', '-O3', '-I', codec, root / 'test_tools/encode_opening_etc.cpp',
                        codec / 'rg_etc1.cpp', '-o', encoder], check=True)
        bake(args.bik_directory.resolve(), staging, encoder)
        staging.rename(target)
