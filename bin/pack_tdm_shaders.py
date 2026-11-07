#!/usr/bin/env python3
"""Rebuild the Android TDM shader patch deterministically; --check verifies it."""
import argparse
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Q3E/src/main/jni/darkmod/glslprogs'
ARCHIVE = ROOT / 'Q3E/src/main/assets/pak/darkmod/glprogs.pk4'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    entries = {'glslprogs/' + p.relative_to(SOURCE).as_posix(): p.read_bytes()
               for p in sorted(SOURCE.rglob('*')) if p.is_file()}
    # The Android ZIP installer historically creates directories only from
    # explicit records. Preserve these for existing installers as well.
    directories = set()
    for name in entries:
        parts = name.split('/')
        directories.update('/'.join(parts[:i]) + '/' for i in range(1, len(parts)))
    entries.update({name: b'' for name in directories})
    if args.check:
        with zipfile.ZipFile(ARCHIVE) as archive:
            actual = {n: archive.read(n) for n in archive.namelist()}
        if actual != entries:
            raise SystemExit('TDM shader archive is stale; run python3 bin/pack_tdm_shaders.py')
        print(f'TDM shader archive matches {len(entries) - len(directories)} sources and {len(directories)} directories')
        return
    with zipfile.ZipFile(ARCHIVE, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o40755 << 16) | 0x10 if name.endswith('/') else 0o100644 << 16
            archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    print(f'Packed {len(entries) - len(directories)} sources and {len(directories)} directories into {ARCHIVE.relative_to(ROOT)}')

if __name__ == '__main__':
    main()
