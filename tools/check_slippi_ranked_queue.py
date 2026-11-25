"""Test the fork against a Slippi Dolphin 3.7.0 macOS bundle on loopback.

Requires Python 3.11+, websockets >= 14 and Xcode command line tools. Example:
  python tools/check_slippi_ranked_queue.py '/Volumes/Slippi/Slippi Dolphin.app'
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess
import tempfile

from websockets.asyncio.server import serve

ROOT = Path(__file__).resolve().parents[1]


async def run(app, work):
    library = app / 'Contents/Frameworks/libslippi_rust_extensions.dylib'
    if not library.is_file():
        raise FileNotFoundError(library)
    client = work / 'clients'
    subprocess.run(['/usr/bin/clang++', '-arch', 'x86_64', '-std=c++17', '-lcurl',
                    '-Iport/runtime/hle', '-Iport/runtime/host', '-Iport/third_party',
                    'port/tests/slippi_ranked_queue_mac.cpp', 'port/runtime/hle/slippi_mm_service.cpp',
                    '-o', str(client)], cwd=ROOT, check=True)
    requests = []
    ready = asyncio.Event()

    async def match(socket):
        join = json.loads(await socket.recv())
        assert socket.request.path == '/v1/queue'
        assert (join['type'], join['appVersion'], join['search']['mode']) == ('join', '3.7.0', 0)
        index = len(requests)
        requests.append(join)
        await socket.send(json.dumps({'type': 'joined', 'ticketId': str(index), 'token': 'local'}))
        if len(requests) == 2:
            ready.set()
        await asyncio.wait_for(ready.wait(), timeout=8)
        assert requests[0].keys() == requests[1].keys()
        assert requests[0]['user'].keys() == requests[1]['user'].keys()
        assert {m['port'] for m in requests} == {49121, 49122}
        players = [
            {'port': seat + 1, 'isLocalPlayer': seat == index, 'uid': f'test-{seat}',
             'displayName': f'Player {seat}', 'connectCode': f'TEST#{seat}',
             'ipAddress': message['ipAddressLan'], 'ipAddressLan': message['ipAddressLan']}
            for seat, message in enumerate(requests)
        ]
        await socket.send(json.dumps({'type': 'match', 'matchId': 'mode.ranked-loopback',
                                      'isHost': index == 0, 'players': players,
                                      'stages': [2, 3, 8, 28, 31, 32]}))
        await socket.wait_closed()

    async with serve(match, '127.0.0.1', 0) as server:
        port = server.sockets[0].getsockname()[1]
        profile = '(version 1)(allow default)(deny network-outbound)(allow network-outbound (remote ip "localhost:*"))'
        process = await asyncio.create_subprocess_exec('/usr/bin/sandbox-exec', '-p', profile,
            str(client), str(library), str(work), f'ws://127.0.0.1:{port}/v1/queue',
            env={'HOME': str(work), 'PATH': os.defpath, 'NO_PROXY': '*'})
        try:
            code = await asyncio.wait_for(process.wait(), timeout=15)
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()
        if code:
            raise SystemExit(code)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dolphin', type=Path)
    with tempfile.TemporaryDirectory() as temporary:
        asyncio.run(run(parser.parse_args().dolphin.resolve(), Path(temporary)))
