"""Prepare compiler inputs locally from the contributor's authorized THUG source checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TEXT = {'.cpp', '.c', '.h', '.hpp', '.inl', '.inc', '.s'}
INCLUDE = re.compile(r'(?m)^(\s*#\s*include\s*[<"])([^>"\n]+)([>"])')

def normalise(raw):
    text = raw.decode('latin1').replace('\r\n', '\n')
    return INCLUDE.sub(lambda m: m[1] + m[2].replace('\\', '/').lower() + m[3], text).encode('latin1')

def source_bytes(image, changes):
    output = bytearray()
    offset = 0
    for start, end, replacements in changes:
        if not offset <= start <= end <= len(image):
            raise ValueError('Invalid local source range')
        output.extend(image[offset:start])
        for value in replacements:
            if isinstance(value, str):
                output.extend(value.encode('latin1'))
            else:
                a,b = value
                if not 0 <= a <= b <= len(image):
                    raise ValueError('Invalid copied source range')
                output.extend(image[a:b])
        offset = end
    output.extend(image[offset:])
    return bytes(output)

def prepare(checkout):
    checkout = checkout.resolve(strict=True)
    ledger = json.loads((ROOT/'tools/source-edits.json').read_text())
    git = ['git','-C',str(checkout)]
    for name, oid in ledger['inputs'].items():
        actual = subprocess.check_output([*git,'rev-parse','HEAD:'+name],text=True).strip()
        if actual != oid:
            raise ValueError(f'Unsupported {name}: expected {oid}, found {actual}')
    subprocess.run([*git,'diff','--quiet','HEAD','--','Code','cmake_files.cmake'],check=True)
    dest = ROOT/'private/original'
    entries = subprocess.check_output([*git,'ls-tree','-rz','--name-only','HEAD','Code']).decode().split('\0')
    seen=set()
    for path in map(Path, filter(None, entries)):
        if path.suffix.lower() not in TEXT:
            continue
        rel=path.relative_to('Code').as_posix().lower()
        if rel in seen or (checkout/path).is_symlink():
            raise ValueError(f'Ambiguous source: {rel}')
        seen.add(rel)
        image = normalise((checkout/path).read_bytes())
        if rel in ledger['edits']:
            patch=ledger['edits'][rel]
            image=source_bytes(image,patch['edits'])
            if hashlib.sha256(image).hexdigest()!=patch['sha256']:
                raise ValueError(f'Unexpected translated input: {rel}')
        target=dest/rel
        target.parent.mkdir(parents=True,exist_ok=True)
        if not target.exists() or target.read_bytes()!=image:
            target.write_bytes(image)
    if not set(ledger['edits'])<=seen or {p.relative_to(dest).as_posix() for p in dest.rglob('*') if p.is_file()}!=seen:
        raise ValueError('Unexpected files in private/original. Use a fresh directory.')
    manifest=(checkout/'cmake_files.cmake').read_text(encoding='latin1')
    names=sorted({s.replace('\\','/').lower() for s in re.findall(r'\$\{SRC_DIR\}/([^"\n]+\.(?:cpp|c))',re.sub(r'(?m)#.*$','',manifest))})
    if not names or any(n not in seen for n in names):
        raise ValueError('Invalid original compiler manifest')
    (ROOT/'private/original-build-sources.cmake').write_text('set(THUG_ORIGINAL_PC_BUILD_SOURCES\n'+''.join(f'    "{n}"\n' for n in names)+')\n')
    print(f'Prepared {len(seen)} local source files and read {len(names)} build entries.')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout',type=Path)
    prepare(parser.parse_args().checkout)
