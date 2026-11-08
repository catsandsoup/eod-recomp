"""Run a real host and guest through two races. Pass eod, disc folder and fresh log directory."""
import os
import re
from pathlib import Path
import subprocess
import sys
import time

exe, disc, root = map(Path, sys.argv[1:])
root.mkdir(parents=True, exist_ok=False)
peers = []
try:
    port = None
    for name in ('host', 'guest'):
        mode = 'host' if name == 'host' else f'join:127.0.0.1:{port}'
        folder = root / name
        folder.mkdir()
        env = dict(os.environ, EOD_NETPLAY_LIFECYCLE='1', EOD_NETPLAY_AUTO=mode, EOD_NETPLAY_TEST_REMATCH='1',
                   EOD_NETPLAY_NO_INTERNET='1', EOD_NETPLAY_NO_LAN='1', EOD_NETPLAY_CAR='Trailer2',
                   EOD_MEMCARD=str(folder/'card'), EOD_APP_SUPPORT=str(folder/'support'),
                   EOD_SKIP_MOVIES='1', EOD_HEADLESS='1', EOD_MUTE='1',
                   EOD_PAD_SCRIPT='400:0008,410:0000', EOD_NETPLAY_DEBUG='1')
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
        if (log.count('[netplay] event ended') != 2 or 'online event 2:' not in log
                or log.count('my car: Trailer2') != 2 or 'event=2 state=lobby native_main=1' not in log):
            raise RuntimeError(f'{name} did not return from both races; see {root/name}')
finally:
    for peer in peers:
        if peer.poll() is None:
            peer.kill()
            peer.wait()
