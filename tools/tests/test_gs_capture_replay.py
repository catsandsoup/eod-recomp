#!/usr/bin/env python3
"""Software-only transport parity with procedural GS packets; no retail data."""
import argparse
import gzip
import os
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--eod', type=Path, required=True)
parser.add_argument('--generator', type=Path, required=True)
parser.add_argument('--work-dir', type=Path, required=True)
a = parser.parse_args()
eod, generator, work = a.eod.resolve(), a.generator.resolve(), a.work_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
config = work / 'capture.ini'
config.write_text('[Video]\nRenderScale=1\n')
fixture = work / 'flat.gsd'
subprocess.run([str(generator), 'flat', str(fixture)], check=True)
raw = fixture.read_bytes()
state_end = 20 + sum(struct.unpack_from('=III', raw, 8)) + 4 * 1024 * 1024
assert raw[-16:] == struct.pack('=IIII', 3, 1, 0, 0)
# Repeat its generated GIF draw at a second vblank; this exercises warm-window
# boundaries without a second fixture generator or any game assets.
raw += raw[state_end:-16] + struct.pack('=IIII', 3, 2, 0, 0)
fixture.write_bytes(raw)
compressed = work / 'flat.gsd.gz'
compressed.write_bytes(gzip.compress(raw))
env = {k: v for k, v in os.environ.items() if not k.startswith('EOD_GS_')}
env.update(EOD_HEADLESS='1', EOD_MUTE='1', EOD_GS_RASTER='0')

def replay(path, label, expected=0, **settings):
    vram = work / (label + '.vram')
    result = subprocess.run([str(eod), '--headless', '--config', str(config),
                             '--renderer', 'software', '--gs-replay', str(path), '--repeat', '1'],
                            env=dict(env, EOD_GS_REPLAY_DUMP=str(vram), **settings),
                            cwd=work, capture_output=True, text=True, timeout=20)
    (work / (label + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode == expected, (label, result.returncode, result.stderr)
    if expected:
        return
    return vram.read_bytes(), Path(str(path) + '.ppm').read_bytes(), result.stderr

base = replay(fixture, 'raw')
stream = replay(compressed, 'gzip')
assert base[:2] == stream[:2], 'gzip changed full VRAM or displayed pixels'
window = replay(compressed, 'window', EOD_GS_REPLAY_MEASURE_AT='1', EOD_GS_REPLAY_STOP_AT='2')
assert base[:2] == window[:2]
assert 'GS render window (1,2]' in window[2]
# A complete gzip stream containing an unfinished GS frame must not produce
# an accepted final screenshot. Explicit stop-at-vblank may safely ignore it.
tail = work / 'tail.gsd.gz'
tail.write_bytes(gzip.compress(raw + struct.pack('=IIII', 4, 0, 0, 0)))
replay(tail, 'tail-rejected', expected=1)
stopped = replay(tail, 'tail-stopped', EOD_GS_REPLAY_STOP_AT='2')
assert stopped[:2] == base[:2]
replay(compressed, 'empty-window', expected=1, EOD_GS_REPLAY_MEASURE_AT='2')
replay(compressed, 'missing-end', expected=1, EOD_GS_REPLAY_STOP_AT='3')
replay(compressed, 'bad-window', expected=1, EOD_GS_REPLAY_MEASURE_AT='2', EOD_GS_REPLAY_STOP_AT='1')
truncated = work / 'truncated.gsd.gz'
truncated.write_bytes(compressed.read_bytes()[:-4])
replay(truncated, 'truncated', expected=1)
print('software raw/gzip full VRAM and pixels match; completed boundaries/windows enforced')
