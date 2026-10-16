#!/usr/bin/env python3
"""Prove range-based pass elimination preserves complete GPU VRAM."""
import argparse, os, re, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser()
for flag in ('eod','generator','work-dir'): p.add_argument('--'+flag,type=Path,required=True)
p.add_argument('--scale',type=int,required=True)
a=p.parse_args();work=a.work_dir.resolve();work.mkdir(parents=True,exist_ok=True)
config=work/'settings.ini';config.write_text(f'[Video]\nRenderScale={a.scale}\nAspectRatio=4:3\n')
env=dict(os.environ,EOD_HEADLESS='1',EOD_MUTE='1',EOD_GS_RASTER='0')
for key in ('EOD_GS_REPLAY_STOP','EOD_GS_REPLAY_TRACE','EOD_GS_DUMP','EOD_SCREENSHOT_SCALE','EOD_SCREENSHOT_WIDTH','EOD_SCREENSHOT_HEIGHT'):env.pop(key,None)
for case in ('alpha_range_rendered','alpha_range_fail_rgb','alpha_range_fail_zb','alpha_range_texture_fail','alpha_range_fail','alpha_range_flat','alpha_range_texture','alpha_range_mixed','alpha_range_mip','alpha_overlap','alpha_depth_strict','alpha_depth_varying'):
    fixture=work/(case+'.gsd');subprocess.run([str(a.generator.resolve()),case,str(fixture)],check=True)
    results=[];draws=[]
    for enabled in (0,1):
        raw=work/f'{case}-{enabled}.vram'
        run=subprocess.run([str(a.eod.resolve()),'--headless','--config',str(config),'--renderer','vulkan','--gs-replay',str(fixture),'--repeat','1'],env=dict(env,EOD_ALPHA_RANGE=str(enabled),EOD_GS_REPLAY_DUMP=str(raw)),capture_output=True,text=True,timeout=30,cwd=work)
        (work/f'{case}-{enabled}.log').write_text(run.stdout+run.stderr)
        assert run.returncode==0,run.stderr
        if 'replaying with vulkan' not in run.stderr:sys.exit(77)
        data=raw.read_bytes();assert len(data)==4*1024*1024;results.append(data)
        draws.append(int(re.search(r'(\d+) draw calls',run.stderr)[1]))
    assert results[0]==results[1],f'{case}: full VRAM differs'
    if case in ('alpha_range_fail_rgb','alpha_range_fail_zb','alpha_range_texture_fail','alpha_range_fail','alpha_range_flat','alpha_range_texture','alpha_depth_strict','alpha_depth_varying'):
        assert draws[1]<draws[0],f'{case}: expected fewer draws with proven constant alpha outcome, got {draws}'
    if case in ('alpha_range_rendered','alpha_range_mixed','alpha_range_mip'):
        assert draws[1]==draws[0],f'{case}: uncertain texture must keep fail draws, got {draws}'
    print(f'{case}: full VRAM identical, draws {draws}',flush=True)
