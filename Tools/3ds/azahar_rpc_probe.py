#!/usr/bin/env python3
"""Read process/performance data and a screenshot from a local Azahar RPC server.

Uses the installed emulator's matching external RPC client; no emulator code is
copied into the game or linked with it. Launch with run_azahar_smoke.py --rpc.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=Path.home() /
                        'Applications/azahar/azahar-macos-arm64-2126.2/scripting/AzaharRPC.py')
    parser.add_argument('--port', type=int, default=45987)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--screenshot', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('installed_azahar_rpc', args.client)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    client = module.AzaharRPC('127.0.0.1', args.port, timeout=2, retries=1)
    result = {'origin': 'Local Azahar RPC; host emulator metrics, not New3DS measurements',
              'timestamp_unix': time.time(), 'client_sha256': hashlib.sha256(args.client.read_bytes()).hexdigest(),
              'private': True, 'errors': {}}
    try:
        for name, operation in [('processes', client.process_list), ('selected_process', client.get_process),
                                ('performance', client.perf_stats)]:
            try:
                result[name] = operation()
            except (OSError, TimeoutError, ValueError, RuntimeError) as error:
                result[name] = None
                result['errors'][name] = str(error)
        if args.screenshot:
            try:
                png = client.screenshot(res_scale=1)
                if not png.startswith(b'\x89PNG\r\n\x1a\n'):
                    raise ValueError('RPC response was not a PNG')
                (args.output/'screen.png').write_bytes(png)
                result['screenshot'] = {'file': 'screen.png', 'sha256': hashlib.sha256(png).hexdigest()}
            except (OSError, TimeoutError, ValueError, RuntimeError) as error:
                result['errors']['screenshot'] = str(error)
    finally:
        client.socket.close()
        (args.output/'rpc.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return int(bool(result['errors']))


if __name__ == '__main__':
    raise SystemExit(main())
