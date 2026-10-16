#!/usr/bin/env python3
"""Compiles runtime/gs/d3d11/gs_shaders.hlsl to gs_shaders.inc (DXBC byte
arrays plus the source).  Uses d3dcompiler_47 through Wine, so it runs on
Linux; the result is committed and the normal build doesn't need it."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "runtime", "gs", "d3d11", "gs_shaders.hlsl")
OUT = os.path.join(ROOT, "runtime", "gs", "d3d11", "gs_shaders.inc")
ENTRIES = ["vs_draw:vs_4_0", "ps_draw:ps_4_0", "vs_pass:vs_4_0", "ps_convert_color:ps_4_0",
           "ps_convert_depth:ps_4_0", "ps_upload_color:ps_4_0", "ps_upload_depth:ps_4_0",
           "ps_readback_color:ps_4_0", "ps_readback_depth:ps_4_0", "ps_copy:ps_4_0",
           "vs_overlay:vs_4_0", "ps_overlay:ps_4_0"]


def main():
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, "shaderc.exe")
    subprocess.check_call(["x86_64-w64-mingw32-g++", "-O1", "-static",
                           os.path.join(ROOT, "tools", "shaderc", "shaderc.cpp"), "-o", exe])
    env = dict(os.environ, WINEDEBUG="-all")
    r = subprocess.run(["wine", exe, SRC, os.path.join(tmp, "out.inc")] + ENTRIES, env=env)
    if r.returncode != 0:
        sys.exit("shader compilation failed")
    data = open(os.path.join(tmp, "out.inc"), "rb").read()
    if not (os.path.exists(OUT) and open(OUT, "rb").read() == data):
        open(OUT, "wb").write(data)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
