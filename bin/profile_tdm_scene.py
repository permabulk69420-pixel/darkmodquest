#!/usr/bin/env python3
"""Capture ABBA profiles of one numeric setting in an already running test game.

Uses TDM's existing automation and Tracy interfaces. The renderer/device recorded
in metadata determine what the results can establish; software-renderer traces
must not be reported as Quest GPU benchmarks. This tool does not infer a win.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
from tdm_automation import TDMClient


def read_cvar(client, name):
    result = client.command(name)
    match = re.search(r'"' + re.escape(name) + r'" is:"([^"]*)"', result)
    if not match:
        raise RuntimeError('Cannot read cvar: ' + result)
    return match[1]


def set_cvar(client, name, value):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
        raise ValueError('Invalid cvar name')
    if not re.fullmatch(r'[-+]?\d+(?:\.\d+)?', str(value)):
        raise ValueError('Comparison supports numeric cvars only')
    client.command(f'set {name} {value}')
    actual = read_cvar(client, name)
    if float(actual) != float(value):
        raise RuntimeError(f'{name} requested {value}, game reports {actual}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=3879)
    parser.add_argument('--tracy-port', type=int, default=8086)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--export', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--setting', required=True)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--seconds', type=int, default=8)
    parser.add_argument('--frontend-only', action='store_true',
                        help='Skip GPU backend to isolate frontend CPU work; not a full-frame benchmark')
    args = parser.parse_args()
    if args.seconds < 3:
        parser.error('--seconds must be at least 3')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', args.setting):
        parser.error('--setting must name one cvar')
    for value in (args.baseline, args.candidate):
        if not re.fullmatch(r'[-+]?\d+(?:\.\d+)?', value):
            parser.error('Comparison values must be numeric')
    args.output.mkdir(parents=True, exist_ok=False)
    client = TDMClient(args.host, args.port)
    original = {}
    metadata = {'setting': args.setting, 'baseline': args.baseline, 'candidate': args.candidate,
                'frontend_only': args.frontend_only, 'seconds': args.seconds, 'runs': []}
    try:
        metadata['status'] = client.request('query', 'status')
        if re.search(r'^guiactive\s+\S', metadata['status'], re.M) or not re.search(r'^mapname\s+\S', metadata['status'], re.M):
            raise RuntimeError('A test mission must be loaded with its menu closed')
        metadata['view'] = client.command('getviewpos').strip()
        metadata['graphics'] = client.command('gfxinfo')
        metadata['version'] = read_cvar(client, 'si_version')
        for name in dict.fromkeys((args.setting, 'g_stopTime', 'r_skipBackEnd', 'com_maxFPS', 'com_speeds')):
            original[name] = read_cvar(client, name)
        metadata['original_settings'] = original.copy()
        if read_cvar(client, 'com_enableTracing') != '1':
            raise RuntimeError('Enable the matching Tracy client with com_enableTracing 1 before capture')
        set_cvar(client, 'g_stopTime', 1)
        set_cvar(client, 'com_speeds', 0)
        if args.frontend_only:
            set_cvar(client, 'r_skipBackEnd', 1)
        elif read_cvar(client, 'r_skipBackEnd') != '0':
            raise RuntimeError('Full-frame capture requires r_skipBackEnd 0')
        set_cvar(client, 'com_maxFPS', 60)
        for index, (label, value) in enumerate((('baseline', args.baseline), ('candidate', args.candidate),
                                                ('candidate', args.candidate), ('baseline', args.baseline)), 1):
            set_cvar(client, args.setting, value)
            time.sleep(2)
            prefix = args.output / f'{index}-{label}'
            trace = prefix.with_suffix('.tracy')
            print(f'Capturing {index}/4: {label}, {args.setting}={value}', flush=True)
            with prefix.with_suffix('.capture.log').open('w') as log:
                subprocess.run([str(args.capture.resolve()), '-a', args.host, '-p', str(args.tracy_port),
                                '-o', str(trace.resolve()), '-s', str(args.seconds)],
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=args.seconds + 30)
            with prefix.with_suffix('.tsv').open('w') as output:
                subprocess.run([str(args.export.resolve()), '-s', '\t', str(trace.resolve())],
                               stdout=output, check=True, timeout=30)
            run = {'label': label, 'value': value, 'trace': trace.name,
                   'view': client.command('getviewpos').strip(), 'status': client.request('query', 'status')}
            if run['view'] != metadata['view'] or run['status'] != metadata['status']:
                raise RuntimeError('Scene or camera changed during comparison; reject these results')
            metadata['runs'].append(run)
    finally:
        for name, value in original.items():
            try:
                set_cvar(client, name, value)
            except (OSError, RuntimeError, ValueError) as error:
                metadata.setdefault('restore_errors', []).append(f'{name}: {error}')
        client.close()
        (args.output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    if metadata.get('restore_errors'):
        raise RuntimeError('Failed to restore test settings; see metadata.json')
    print('Saved four raw traces, zone statistics and machine/scene/settings metadata.', flush=True)


if __name__ == '__main__':
    main()
