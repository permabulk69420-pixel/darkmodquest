#!/usr/bin/env python3
"""Build the Quest ARM64 native engine with its real OpenXR loader (no stub).

Example: python3 bin/check_quest_renderer.py --ndk /path/to/android-ndk-r23c \
    --openxr-prefab /path/to/openxr_loader_for_android-1.0.34/prefab
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
JNI = ROOT / 'Q3E/src/main/jni'

def run(*command):
    subprocess.run([str(c) for c in command], check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT'))
    parser.add_argument('--openxr-prefab', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/quest-arm64')
    parser.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument('--cmake', default='cmake')
    args = parser.parse_args()
    if not args.ndk or not (args.ndk / 'build/cmake/android.toolchain.cmake').is_file():
        parser.error('--ndk must point to the Android NDK (repository pin: 23.2.8568313)')
    loader = args.openxr_prefab / 'modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so'
    if not loader.is_file():
        parser.error('--openxr-prefab must contain the extracted OpenXR 1.0.34 ARM64 loader')
    run(sys.executable, ROOT / 'bin/pack_tdm_shaders.py', '--check')
    # These are the same game switches used by the APK CI, including future
    # additions. Keep the current checkout; a cloud task is already isolated.
    games = re.findall(r'^OPTION\((GAME_\w+)', (JNI / 'CMakeLists.txt').read_text(), re.M | re.I)
    disabled = ['-D'+name+'=OFF' for name in sorted(set(games)) if name != 'GAME_TDM']
    build = args.build_dir.resolve()
    run(args.cmake, '-S', JNI, '-B', build, '-G', 'Ninja',
        '-DCMAKE_TOOLCHAIN_FILE='+str(args.ndk.resolve() / 'build/cmake/android.toolchain.cmake'),
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-21',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DTDM_OPENXR_PREFAB='+str(args.openxr_prefab.resolve()),
        '-DDIII4A_PRODUCT_FLAVOR=idtech', '-DGAME_TDM=ON', *disabled)
    commands = json.loads((build / 'compile_commands.json').read_text())
    xr = next(c for c in commands if c['file'].endswith('/renderer/vr/OpenXRBackend.cpp'))
    forward = next(c for c in commands if c['file'].endswith('/MobileLightingStage.cpp'))
    for entry in (xr, forward):
        if '-DTDM_VR_OPENXR' not in entry['command']:
            raise SystemExit('OpenXR integration was stubbed out; this is not a valid Quest build')
        if '-O2' not in entry['command']:
            raise SystemExit('Quest Release build must enable optimization')
    run(args.cmake, '--build', build, '--target', 'TheDarkMod', '-j', max(1,args.jobs))
    engine = build / 'darkmod/libTheDarkMod.so'
    readelf = args.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf'
    header = subprocess.check_output([str(readelf), '-h', str(engine)], text=True)
    needed = subprocess.check_output([str(readelf), '-d', str(engine)], text=True)
    if 'AArch64' not in header or 'libopenxr_loader.so' not in needed:
        raise SystemExit('Wrong architecture or missing OpenXR dependency in the linked engine')
    print('PASS: optimized Android ARM64 native engine with GLES and OpenXR:', engine)

if __name__ == '__main__':
    main()
