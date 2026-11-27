#!/usr/bin/env python3
"""Compiles runtime/render/native_shaders.hlsl (the native renderer's shaders) to SPIR-V:
runtime/render/native_shaders_spv.inc (uint32 arrays, committed; the normal build does not
need the compiler).

Needs DXC (DirectXShaderCompiler) with SPIR-V output: set DXC to its path or put dxc on PATH.
plume pins one (examples/cmake/modules/PlumeDXC.cmake: renderbag/dxc-bin 7810655). Each entry
is compiled on its own (shader model 6.0, Vulkan 1.1 target, -O3). The pixel shader's second
output is moved to location 0 index 1 (dual-source blending), as tools/build_vk_shaders.py does
for the GS shaders.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "runtime", "render", "native_shaders.hlsl")
OUT = os.path.join(ROOT, "runtime", "render", "native_shaders_spv.inc")

ENTRIES = [("vs_main", "vs"), ("ps_main", "ps"), ("vs_static_queue", "vs")]
# stage R6: the Modern look, its own file so the Classic SPIR-V never changes with it
MODERN_SRC = os.path.join(ROOT, "runtime", "render", "native_modern.hlsl")
MODERN_OUT = os.path.join(ROOT, "runtime", "render", "native_modern_spv.inc")
MODERN_ENTRIES = [
    ("vs_modern", "vs"),
    ("vs_vfx_billboard", "vs"),
    ("ps_modern", "ps"),
    ("ps_modern_scene", "ps"),
    ("ps_modern_opaque", "ps"),
    ("ps_modern_scene_opaque", "ps"),
    ("ps_modern_water_opaque", "ps"),
    ("ps_modern_unlit_opaque", "ps"),
    ("ps_modern_pbr_ate", "ps"),
    ("ps_modern_scene_ate", "ps"),
    ("ps_modern_unlit_ate", "ps"),
    ("ps_modern_depth_only", "ps"),
]
SUN_SRC = os.path.join(ROOT, "runtime", "render", "native_sun_cascades.hlsl")
SUN_OUT = os.path.join(ROOT, "runtime", "render", "native_sun_cascades_spv.inc")
SUN_ENTRIES = [
    ("vs_sun_caster", "vs"),
    ("vs_sun_caster_ate", "vs"),
    ("ps_sun_caster_ate", "ps"),
    ("vs_sun_receiver", "vs"),
    ("ps_sun_receiver", "ps"),
]
JOBS = {"classic": (SRC, OUT, ENTRIES), "modern": (MODERN_SRC, MODERN_OUT, MODERN_ENTRIES),
        "sun": (SUN_SRC, SUN_OUT, SUN_ENTRIES)}


def main():
    """usage: build_native_shaders.py [classic] [modern] [sun] (default: all)"""
    for name in sys.argv[1:] or list(JOBS):
        build(*JOBS[name])


def _run(cmd, label):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("failed: %s\n%s%s" % (label, r.stdout, r.stderr))
    return r.stdout


def _compile_glslang(SRC, ENTRIES, tmp):
    glslang = os.environ.get("GLSLANG") or shutil.which("glslangValidator")
    spirv_dis = shutil.which("spirv-dis")
    spirv_as = shutil.which("spirv-as")
    spirv_opt = shutil.which("spirv-opt")
    spirv_val = shutil.which("spirv-val")
    if not glslang:
        sys.exit("neither dxc nor glslangValidator found")
    # glslang's HLSL front end expects ConstantBuffer<T> for [[vk::push_constant]] structs
    # and does not support [[vk::index(1)]] inline (patched onto location 0 index 1 below).
    src_text = open(SRC, "r", encoding="utf-8").read()
    src_text = re.sub(
        r"\[\[vk::push_constant\]\]\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;",
        r"[[vk::push_constant]] ConstantBuffer<\1> \2;",
        src_text,
    )
    has_dual = "[[vk::location(0), vk::index(1)]]" in src_text
    if has_dual:
        src_text = src_text.replace("[[vk::location(0), vk::index(1)]]", "[[vk::location(1)]]")
    patched_src = os.path.join(tmp, os.path.basename(SRC))
    with open(patched_src, "w", encoding="utf-8", newline="\n") as f:
        f.write(src_text)

    stage_map = {"vs": "vert", "ps": "frag", "cs": "comp"}
    results = []
    for entry, stage in ENTRIES:
        spv = os.path.join(tmp, entry + ".spv")
        _run([glslang, "-D", "-V", "-S", stage_map[stage], "-e", entry,
              "--target-env", "vulkan1.1", "-g0", patched_src, "-o", spv], entry)
        if stage == "ps" and has_dual:
            if not (spirv_dis and spirv_as):
                sys.exit("spirv-dis and spirv-as are required for dual-source blending with glslangValidator")
            asm = _run([spirv_dis, "--raw-id", "--no-header", spv], entry + " dis")
            outputs = set(re.findall(r"%(\d+) = OpVariable %\d+ Output", asm))
            ids = [i for i in re.findall(r"OpDecorate %(\d+) Location 1\b", asm) if i in outputs]
            if len(ids) != 1:
                sys.exit("%s: expected one location-1 output, found %d" % (entry, len(ids)))
            vid = ids[0]
            asm = asm.replace("OpDecorate %%%s Location 1\n" % vid,
                              "OpDecorate %%%s Location 0\nOpDecorate %%%s Index 1\n" % (vid, vid))
            spvasm = os.path.join(tmp, entry + ".spvasm")
            with open(spvasm, "w", encoding="utf-8", newline="\n") as f:
                f.write(asm)
            _run([spirv_as, "--target-env", "vulkan1.1", spvasm, "-o", spv], entry + " as")
        if spirv_opt:
            opt_spv = os.path.join(tmp, entry + ".opt.spv")
            _run([spirv_opt, "-O", "--target-env=vulkan1.1", spv, "-o", opt_spv], entry + " opt")
            spv = opt_spv
        if spirv_val:
            _run([spirv_val, "--target-env", "vulkan1.1", spv], entry + " val")
        data = open(spv, "rb").read()
        results.append((entry, [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]))
    return results


def build(SRC, OUT, ENTRIES):
    dxc = os.environ.get("DXC") or shutil.which("dxc")
    tmp = tempfile.mkdtemp()
    out = ["// generated by tools/build_native_shaders.py from runtime/render/%s - do not edit" % os.path.basename(SRC)]
    try:
        if dxc:
            entries_words = []
            for entry, stage in ENTRIES:
                spv = os.path.join(tmp, entry + ".spv")
                cmd = [dxc, "-spirv", "-T", stage + "_6_0", "-E", entry, "-fspv-target-env=vulkan1.1",
                       "-fvk-use-dx-layout", "-O3", SRC, "-Fo", spv]
                _run(cmd, entry)
                data = open(spv, "rb").read()
                entries_words.append((entry, [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]))
        else:
            entries_words = _compile_glslang(SRC, ENTRIES, tmp)
        for entry, words in entries_words:
            out.append("[[maybe_unused]] static const uint32_t k_native_%s[] = {" % entry)
            for i in range(0, len(words), 8):
                out.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
            out.append("};")
    finally:
        shutil.rmtree(tmp)
    text = "\n".join(out) + "\n"
    if not (os.path.exists(OUT) and open(OUT).read() == text):
        with open(OUT, "w", newline="\n") as f:
            f.write(text)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
