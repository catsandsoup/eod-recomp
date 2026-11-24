"""Run a real host and guest through two races. Pass eod, disc folder and fresh log directory."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("exe", type=Path)
parser.add_argument("disc", type=Path)
parser.add_argument("log_dir", type=Path)
parser.add_argument("--reject-trailer", action="store_true",
                    help="Request a standalone trailer and verify that both seats use drivable cars.")
args = parser.parse_args()
exe, disc, root = (path.resolve() for path in (args.exe, args.disc, args.log_dir))
root.mkdir(parents=True, exist_ok=False)
peers = []
try:
    port = None
    for name in ('host', 'guest'):
        mode = 'host' if name == 'host' else f'join:127.0.0.1:{port}'
        folder = root / name
        folder.mkdir()
        env = dict(os.environ, EOD_NETPLAY_LIFECYCLE='1', EOD_NETPLAY_AUTO=mode, EOD_NETPLAY_TEST_REMATCH='1',
                   EOD_NETPLAY_NO_INTERNET='1', EOD_NETPLAY_NO_LAN='1',
                   EOD_NETPLAY_CAR='Trailer2' if args.reject_trailer else 'Moth',
                   EOD_MEMCARD=str(folder/'card'), EOD_APP_SUPPORT=str(folder/'support'),
                   EOD_SKIP_MOVIES='1', EOD_HEADLESS='1', EOD_MUTE='1',
                   EOD_PAD_SCRIPT='400:0008,410:0000', EOD_NETPLAY_DEBUG='1')
        env.setdefault('EOD_LOG', '-game')
        with (folder/'game.log').open('w') as log:
            peers.append(subprocess.Popen([exe, '--headless', '--data', disc, '--config',
                folder/'eod.ini', '--frames', '4600'], env=env, stdout=log, stderr=log))
        if name == 'host':
            for _ in range(300):
                match = re.search(r'hosting on [^:]+:(\d+),', (folder/'game.log').read_text())
                if match:
                    port = match[1]
                    break
                time.sleep(0.1)
            if port is None:
                raise RuntimeError('test host did not open a port')
    for peer in peers:
        if peer.wait(timeout=180):
            raise RuntimeError('game failed')
    for name in ('host','guest'):
        log = (root/name/'game.log').read_text()
        cars = re.findall(r'^\[netplay\] my car: (.+) \(#(\d+)\)$', log, re.MULTILINE)
        if len(cars) != 2 or cars[0] != cars[1]:
            raise RuntimeError(f'{name} did not keep the same vehicle across both races')
        model, number = cars[0]
        if args.reject_trailer and (model.lower().startswith('trailer') or 400 <= int(number) <= 404):
            raise RuntimeError(f'{name} started with an attachment instead of a drivable car')
        if not args.reject_trailer and model != 'Moth':
            raise RuntimeError(f'{name} did not retain the selected Moth')
        if (log.count('[netplay] GO (slot ') != 2 or log.count('[netplay] event ended') != 2 or 'online event 2:' not in log
                or 'event=2 state=lobby native_main=1' not in log):
            raise RuntimeError(f'{name} did not return from both races; see {root/name}')
finally:
    for peer in peers:
        if peer.poll() is None:
            peer.kill()
            peer.wait()
