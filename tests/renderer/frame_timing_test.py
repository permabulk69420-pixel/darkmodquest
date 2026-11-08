#!/usr/bin/env python3
"""Compile production timing/reporting with deterministic clocks and GL/XR APIs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / 'Q3E/src/main/jni/darkmod'

def main():
    headers = Path(os.environ.get('TDM_OPENXR_HEADERS', '/tmp/renderer-tools/openxr/prefab/modules/headers/include'))
    if not (headers / 'openxr/openxr.h').is_file():
        raise SystemExit('Set TDM_OPENXR_HEADERS to the APK OpenXR loader headers')
    with tempfile.TemporaryDirectory(prefix='quest-frame-timing-') as directory:
        binary = Path(directory) / 'timing'
        subprocess.run(['g++', '-std=c++14', '-O2', '-Wall', '-Wextra', '-Wno-missing-field-initializers',
                        '-I', str(ENGINE), '-I', str(headers), str(Path(__file__).with_suffix('.cpp')), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
