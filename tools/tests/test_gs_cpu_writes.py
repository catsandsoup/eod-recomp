#!/usr/bin/env python3
"""Compare complete GS VRAM after synthetic CPU/GPU transfer ordering cases."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
p=argparse.ArgumentParser()
p.add_argument('--eod',type=Path,required=True);p.add_argument('--generator',type=Path,required=True);p.add_argument('--work-dir',type=Path,required=True)
p.add_argument('--scale',type=int,choices=(1,6),default=1)
a=p.parse_args();exe=a.eod.resolve();generator=a.generator.resolve();work=a.work_dir.resolve();work.mkdir(parents=True,exist_ok=True)
config=work/'cpu-writes.ini';config.write_text(f'[Video]\nRenderScale={a.scale}\nAspectRatio=4:3\n')
env=dict(os.environ,EOD_HEADLESS='1',EOD_MUTE='1',EOD_GS_RASTER='0')
for key in ('EOD_SCREENSHOT_WIDTH','EOD_SCREENSHOT_HEIGHT','EOD_SCREENSHOT_SCALE','EOD_GS_REPLAY_STOP','EOD_GS_REPLAY_TRACE','EOD_GS_DUMP','EOD_GS_READBACK_TRACE'):
    env.pop(key,None)
for case in ('partial','neighbor','pending','full','chunks','interrupted','ct24','high8','high4lo','high4hi','localcopy','multi_partial',
             'gpu_blocks_color32','gpu_blocks_color24','gpu_blocks_color16','gpu_blocks_color16s',
             'gpu_blocks_depth32','gpu_blocks_depth24','gpu_blocks_depth16','gpu_blocks_depth16s',
             'gpu_blocks_color32_draw','gpu_blocks_color32_cpu_read'):
    fixture=work/(case+'.gsd');subprocess.run([str(generator),case,str(fixture)],check=True)
    results={}
    for renderer in ('software','vulkan'):
        raw=work/(case+'-'+renderer+'.vram');env['EOD_GS_REPLAY_DUMP']=str(raw)
        run=subprocess.run([str(exe),'--headless','--config',str(config),'--renderer',renderer,'--gs-replay',str(fixture),'--repeat','1','--log','gs'],env=env,cwd=work,capture_output=True,text=True,timeout=30)
        (work/(case+'-'+renderer+'.log')).write_text(run.stdout+run.stderr)
        if run.returncode:raise RuntimeError(run.stderr[-2000:])
        if renderer=='vulkan' and 'replaying with vulkan' not in run.stderr:
            print('SKIP: no Vulkan device, software fallback does not verify GPU ordering');sys.exit(77)
        results[renderer]=raw.read_bytes();assert len(results[renderer])==4*1024*1024
    if results['software']!=results['vulkan']:
        differences=[i for i,(x,y) in enumerate(zip(results['software'],results['vulkan'])) if x!=y]
        raise AssertionError(f'{case}: {len(differences)} GS bytes differ; first {differences[:12]}')
    print(case+': complete GS VRAM matches software',flush=True)
    if case.startswith('gpu_blocks_'):
        probe_env=dict(env);probe_env.pop('EOD_GS_REPLAY_DUMP',None)
        run=subprocess.run([str(exe),'--headless','--config',str(config),'--renderer','vulkan','--gs-replay',str(fixture),'--repeat','1'],env=probe_env,cwd=work,capture_output=True,text=True,timeout=30)
        (work/(case+'-no-cpu-read.log')).write_text(run.stdout+run.stderr)
        assert run.returncode==0 and 'replaying with vulkan' in run.stderr
        assert 'readbacks for CPU 0 ' in run.stderr, (case,'GPU upload must not require a CPU preservation read',run.stderr[-1600:])
print('GPU transfer ordering, full chunks, interrupted transfers and masked storage preservation passed')
