#!/usr/bin/env python3
"""Control an opted-in TDM test instance through its existing automation API.

Start the game with +set com_automation 1, then use --query status or
--command 'getviewpos'. No changes to the engine or gameplay controls are needed.
"""
import argparse
from pathlib import Path
import socket
import struct


class TDMClient:
    def __init__(self, host='127.0.0.1', port=3879, timeout=60):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.seqno = 0

    def close(self):
        self.socket.close()

    def _receive(self, size):
        data = bytearray()
        while len(data) < size:
            part = self.socket.recv(size - len(data))
            if not part:
                raise ConnectionError('TDM closed the automation connection')
            data.extend(part)
        return bytes(data)

    def request(self, kind, name, content=''):
        if kind not in ('action', 'query') or not name.replace('-', '').isalnum():
            raise ValueError('Invalid automation request')
        self.seqno += 1
        payload = (f'seqno {self.seqno}\nmessage "{kind}"\n{kind} "{name}"\n'
                   f'content:\n{content}\n').encode('utf-8')
        length = struct.pack('<I', len(payload))
        self.socket.sendall(b'TDM[' + length + b']   ' + payload + b'   (' + length + b')TDM')
        header = self._receive(12)
        if header[:4] != b'TDM[' or header[8:] != b']   ':
            raise ValueError('Invalid TDM response header')
        size = struct.unpack_from('<I', header, 4)[0]
        if size > 16 * 1024 * 1024:
            raise ValueError('TDM response exceeds 16 MiB')
        response = self._receive(size).decode('utf-8', errors='replace')
        if self._receive(12) != b'   (' + struct.pack('<I', size) + b')TDM':
            raise ValueError('Invalid TDM response footer')
        prefix = f'response {self.seqno}\n'
        if not response.startswith(prefix):
            raise ValueError('TDM response belongs to another request')
        return response[len(prefix):]

    def command(self, command):
        return self.request('action', 'conexec', command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=3879)
    parser.add_argument('--timeout', type=float, default=60)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--query', choices=('status', 'fps', 'console'))
    mode.add_argument('--command', action='append', help='Console command; repeat to run several')
    mode.add_argument('--commands-file', type=Path)
    mode.add_argument('--action', choices=('installfm', 'guiscript', 'sysctrl'))
    parser.add_argument('--content', default='')
    args = parser.parse_args()
    client = TDMClient(args.host, args.port, args.timeout)
    try:
        if args.command or args.commands_file:
            commands = '\n'.join(args.command) if args.command else args.commands_file.read_text()
            result = client.command(commands)
        else:
            result = client.request('query' if args.query else 'action', args.query or args.action, args.content)
        print(result, end='' if result.endswith('\n') else '\n')
    finally:
        client.close()


if __name__ == '__main__':
    main()
