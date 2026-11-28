#!/usr/bin/env python3
"""Generate an interactive browser-viewable FULL-GAME engine decompilation progression map.

Reads:
  - gen/table.c            (ALL 13,481 functions in SLUS_209.10 — honest full-binary total)
  - src/*.cpp              (live diff_run(0x...) byte-exact decompiled functions)
  - config/hle.txt         (all HLE-wrapped / replaced functions and their comments)
  - config/symbols.txt     (named EE symbols)
  - docs/decomp_roadmap.json (task queue, descriptions, tags, verification stats)

Writes:
  - docs/decomp_progression.html
"""
from __future__ import annotations

import json
import re
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN_TABLE = ROOT / "gen" / "table.c"
SRC_DIR = ROOT / "src"
HLE_FILE = ROOT / "config" / "hle.txt"
SYM_FILE = ROOT / "config" / "symbols.txt"
ROADMAP_FILE = ROOT / "docs" / "decomp_roadmap.json"
OUT_HTML = ROOT / "docs" / "decomp_progression.html"

# Honest full-binary address bands across SLUS_209.10 (0x00100000 .. 0x003B0000+)
ELF_BANDS = [
    (0x00100000, 0x00130000, "0x100k–12F · Core Loop, Events, HUD, Memory Allocator"),
    (0x00130000, 0x00160000, "0x130k–15F · Config, Options, Track/Event Tables, Util"),
    (0x00160000, 0x00180000, "0x160k–17F · Battle, Race Lifecycle, Chicken Weapons, Contact"),
    (0x00180000, 0x001C0000, "0x180k–1BF · Vehicle Physics, Damage, Turbo, AI, Recovery"),
    (0x001C0000, 0x00200000, "0x1C0k–1FF · Camera, Swept Collision, Debris, Deformation"),
    (0x00200000, 0x00240000, "0x200k–23F · Scene Graph, Visual Cars, World/LOD, Sky, Car Audio"),
    (0x00240000, 0x002E0000, "0x240k–2DF · UI Engine, MenuRun, Screens, Front-End"),
    (0x002E0000, 0x00300000, "0x2E0k–2FF · Career Progression, Dealership, Garage, Upgrades"),
    (0x00300000, 0x00340000, "0x300k–33F · Flares, Water, Fog, Streamer, Timers"),
    (0x00340000, 0x00370000, "0x340k–36F · Render Queue, VU1/GS Packets, Particles, Shadows"),
    (0x00370000, 0x00500000, "0x370k+ · Sony PS2 SDK, libgcc, newlib CRT (HLE/Native Replaced)"),
]


def parse_all_elf_funcs() -> list[int]:
    if not GEN_TABLE.exists():
        return []
    addrs: list[int] = []
    pat = re.compile(r"0x([0-9A-Fa-f]{8}),\s*f_")
    for m in pat.finditer(GEN_TABLE.read_text(encoding="utf-8", errors="replace")):
        addrs.append(int(m.group(1), 16))
    return sorted(set(addrs))


def parse_symbols() -> dict[str, str]:
    syms: dict[str, str] = {}
    if not SYM_FILE.exists():
        return syms
    for line in SYM_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
        if m:
            syms[m.group(1).upper()] = m.group(2)
    return syms


def parse_hle() -> dict[str, dict]:
    hle: dict[str, dict] = {}
    if not HLE_FILE.exists():
        return hle
    for line in HLE_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^\s*([0-9A-Fa-f]{8})\s+(wrap|replace|idle)\s*(?:#\s*(.*))?$", line)
        if m:
            addr = m.group(1).upper()
            hle[addr] = {
                "mode": m.group(2),
                "comment": (m.group(3) or "").strip(),
            }
    return hle


def parse_diff_runs() -> dict[str, dict]:
    found: dict[str, dict] = {}
    pat = re.compile(r"diff_run\s*\(\s*0x([0-9A-Fa-f]+)[uU]?\s*,\s*([^,\s\)]+)")
    for cpp in sorted(SRC_DIR.glob("*.cpp")):
        text = cpp.read_text(encoding="utf-8", errors="replace")
        for m in pat.finditer(text):
            addr = m.group(1).upper().zfill(8)
            found[addr] = {"file": f"src/{cpp.name}", "cpp_fn": m.group(2)}
    return found


def classify_file_subsystem(filename: str) -> tuple[str, list[str]]:
    name = filename.lower()
    if "render_queue" in name or "render_trace" in name:
        return "Rendering: Draw Queue & Submission", ["rendering"]
    if "native_world" in name:
        return "Rendering: Track World, Sky & Water", ["rendering"]
    if "native_effects" in name:
        return "Rendering: Particles, Sprites & Immediate Geo", ["rendering"]
    if "native_shadows" in name:
        return "Rendering: Shadows & Flares", ["rendering"]
    if "native_fog" in name or "native_tex" in name or "native_lod" in name or "hle_lod" in name:
        return "Rendering: Fog, Textures & LOD", ["rendering"]
    if "decomp_graphics_env" in name:
        return "Rendering: Lighting, Sky & Environment State", ["rendering"]
    if "bone_pose" in name:
        return "Animation & Car Deformation", ["rendering", "physics"]
    if "car_damage" in name or "debris_pool" in name:
        return "Vehicle Damage & Debris Pools", ["mods", "physics"]
    if "decomp_vehicle" in name:
        return "Vehicle Control & Online Impact", ["online", "mods", "physics"]
    if "decomp_race" in name:
        return "Online Session & Race Lifecycle", ["online", "mods"]
    return "Other Decompiled Engine", ["core"]


def classify_hle_subsystem(comment: str) -> tuple[str, list[str]]:
    c = comment.lower()
    if any(k in c for k in ("online", "netplay", "replica", "peer", "rng", "detentionmaster", "crash orbit")):
        return "Online Session & Race Lifecycle", ["online"]
    if "chicken" in c:
        return "Explosive Chicken Weapons", ["online", "mods", "weapons"]
    if "battle" in c or "ai" in c:
        return "Battle Mode & AI Bots", ["mods", "battle"]
    if any(k in c for k in ("career", "dealer", "garage", "homebase", "autoshop", "challenger", "part")):
        return "Career & Garage Economy", ["mods", "career"]
    if any(k in c for k in ("nascar", "esfx", "audio", "transmission")):
        return "UI, Camera & NASCAR Audio", ["mods", "audio"]
    if any(k in c for k in ("hud", "menu", "camera", "screen", "movie", "loading", "direct start", "label")):
        return "UI, Camera & Session Flow", ["ui"]
    if any(k in c for k in ("wheel", "chassis", "portable map", "damage", "invincibility", "turbo", "recovery", "panel", "debris")):
        return "Vehicle Physics & Custom Maps", ["mods", "physics"]
    if any(k in c for k in ("dma", "vu1", "gs", "texture", "shadow", "fog", "lod", "mlist", "graf", "viewport", "water", "sky", "particle", "sprite", "packet", "visual-car")):
        return "Rendering & GS/VU1 Pipeline", ["rendering"]
    if any(k in c for k in ("memalloc", "memfree", "printf", "panic", "strm", "timer", "vblank", "busy-wait", "dmasync")):
        return "Kernel, Memory & I/O", ["core"]
    return "Other Engine HLE", ["core"]


def build_data() -> dict:
    all_elf = parse_all_elf_funcs()
    syms = parse_symbols()
    hle = parse_hle()
    diffs = parse_diff_runs()
    roadmap = json.loads(ROADMAP_FILE.read_text(encoding="utf-8"))

    rm_by_addr: dict[str, dict] = {}
    for task in roadmap["tasks"]:
        for fn in task["functions"]:
            addr = fn["addr"].upper().zfill(8)
            rm_by_addr[addr] = {
                **fn,
                "addr": addr,
                "task_id": task["id"],
                "task_title": task["title"],
                "task_status": task["status"],
                "subsystem": task["subsystem"],
            }

    # Full-binary address band breakdown (100% honest across all 13,481 ELF functions)
    bands_out = []
    for lo, hi, label in ELF_BANDS:
        in_band = [a for a in all_elf if lo <= a < hi]
        done_b = sum(1 for a in in_band if f"{a:08X}" in diffs)
        prog_b = sum(
            1
            for a in in_band
            if f"{a:08X}" not in diffs
            and f"{a:08X}" in rm_by_addr
            and rm_by_addr[f"{a:08X}"]["task_status"] == "in_progress"
        )
        prio_b = sum(
            1
            for a in in_band
            if f"{a:08X}" not in diffs
            and f"{a:08X}" in rm_by_addr
            and rm_by_addr[f"{a:08X}"]["task_status"] != "in_progress"
        )
        hle_b = sum(
            1
            for a in in_band
            if f"{a:08X}" not in diffs
            and f"{a:08X}" not in rm_by_addr
            and f"{a:08X}" in hle
        )
        raw_b = len(in_band) - done_b - prog_b - prio_b - hle_b
        bands_out.append({
            "label": label,
            "lo": f"0x{lo:08X}",
            "hi": f"0x{hi:08X}",
            "is_sdk": lo >= 0x00370000,
            "total": len(in_band),
            "done": done_b,
            "in_progress": prog_b,
            "queued_priority": prio_b,
            "hle_wrapped": hle_b,
            "raw_recomp": raw_b,
        })

    # Detailed function entries (currently tracked: diff_run + roadmap + HLE)
    tracked_addrs = sorted(set(diffs.keys()) | set(rm_by_addr.keys()) | set(hle.keys()))
    entries: list[dict] = []
    for addr in tracked_addrs:
        is_done = addr in diffs
        rm = rm_by_addr.get(addr)
        h = hle.get(addr)
        sym = syms.get(addr, "")
        if not is_done and not rm and h and h["mode"] in ("idle", "replace"):
            continue
        if is_done:
            status = "done"
        elif rm and rm["task_status"] == "in_progress":
            status = "in_progress"
        elif rm:
            status = "queued_priority"
        else:
            status = "queued_other"

        if rm:
            subsystem = rm["subsystem"]
            tags = list(rm.get("tags", []))
            name = rm.get("name") or sym or (diffs[addr]["cpp_fn"] if is_done else f"f_{addr}")
            what = rm.get("what") or (h["comment"] if h else "")
            task_id = rm["task_id"]
        elif is_done:
            subsystem, tags = classify_file_subsystem(diffs[addr]["file"])
            name = sym or diffs[addr]["cpp_fn"]
            what = h["comment"] if h and h["comment"] else f"Decompiled C++ replacement ({diffs[addr]['cpp_fn']}) verified via EOD_DIFF."
            task_id = "BASE"
        else:
            subsystem, tags = classify_hle_subsystem(h["comment"] if h else "")
            name = sym or f"f_{addr}"
            what = h["comment"] if h and h["comment"] else "Hooked engine function (recompiled body not yet replaced with C++)."
            task_id = ""

        entries.append({
            "addr": f"0x{addr}",
            "hex": addr,
            "name": name,
            "what": what,
            "subsystem": subsystem,
            "tags": sorted(set(tags)),
            "status": status,
            "file": diffs[addr]["file"] if is_done else "",
            "cpp_fn": diffs[addr]["cpp_fn"] if is_done else "",
            "task_id": task_id,
            "hle_comment": h["comment"] if h else "",
            "diff_calls": rm.get("diff_calls", "") if rm else "",
            "diff_mismatches": rm.get("diff_mismatches", 0 if is_done else "") if rm else (0 if is_done else ""),
        })

    tasks_out = []
    for task in roadmap["tasks"]:
        fns = []
        done_cnt = 0
        for fn in task["functions"]:
            addr = fn["addr"].upper().zfill(8)
            fn_done = addr in diffs
            if fn_done:
                done_cnt += 1
            fns.append({
                **fn,
                "addr": f"0x{addr}",
                "done": fn_done,
                "file": diffs[addr]["file"] if fn_done else "",
            })
        t_status = "done" if (done_cnt == len(fns) and len(fns) > 0) else task["status"]
        tasks_out.append({
            "id": task["id"],
            "title": task["title"],
            "subsystem": task["subsystem"],
            "priority": task["priority"],
            "tags": task["tags"],
            "status": t_status,
            "commit": task.get("commit", ""),
            "notes": task.get("notes", ""),
            "done_count": done_cnt,
            "total_count": len(fns),
            "functions": fns,
        })

    total_elf = len(all_elf)
    game_elf = sum(1 for a in all_elf if a < 0x00370000)
    sdk_elf = total_elf - game_elf
    done_game = sum(1 for a in all_elf if a < 0x00370000 and f"{a:08X}" in diffs)

    return {
        "project": roadmap["project"],
        "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M"),
        "total_elf_funcs": total_elf,
        "game_elf_funcs": game_elf,
        "sdk_elf_funcs": sdk_elf,
        "done_total": len(diffs),
        "done_game": done_game,
        "symbols_count": len(syms),
        "hle_count": len(hle),
        "bands": bands_out,
        "entries": entries,
        "tasks": tasks_out,
    }


HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<title>EOD Full-Game Engine Decompilation Progression Map</title>
<style>
  :root {
    --bg: #0b0f17;
    --panel: #131a26;
    --panel-2: #192233;
    --border: #263248;
    --text: #e6edf7;
    --muted: #8ea0bc;
    --done: #22c55e;
    --done-bg: rgba(34, 197, 94, 0.16);
    --prog: #f59e0b;
    --prog-bg: rgba(245, 158, 11, 0.18);
    --prio: #38bdf8;
    --prio-bg: rgba(56, 189, 248, 0.14);
    --other: #64748b;
    --other-bg: rgba(100, 116, 139, 0.16);
    --raw: #1e293b;
  }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Inter, sans-serif; background: var(--bg); color: var(--text); line-height: 1.45; }
  header { padding: 22px 28px 18px; border-bottom: 1px solid var(--border); background: linear-gradient(180deg, #121a2a 0%, var(--bg) 100%); display: flex; flex-wrap: wrap; justify-content: space-between; align-items: flex-end; gap: 14px; }
  h1 { margin: 0; font-size: 21px; font-weight: 700; }
  .subtitle { color: var(--muted); font-size: 13px; margin-top: 4px; }
  .timestamp { font-size: 12px; color: var(--muted); font-family: ui-monospace, Consolas, monospace; }
  main { max-width: 1440px; margin: 0 auto; padding: 20px 28px 60px; }
  .kpis { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 14px; margin-bottom: 18px; }
  .kpi { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 16px 18px; }
  .kpi-label { font-size: 11.5px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.06em; }
  .kpi-val { font-size: 26px; font-weight: 800; margin-top: 6px; font-variant-numeric: tabular-nums; }
  .kpi-sub { font-size: 12px; color: var(--muted); margin-top: 4px; }
  .bar-card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 16px 18px; margin-bottom: 16px; }
  .bar-header { display: flex; justify-content: space-between; font-size: 13px; margin-bottom: 8px; flex-wrap: wrap; gap: 8px; }
  .stacked-bar { height: 15px; border-radius: 999px; background: var(--raw); overflow: hidden; display: flex; }
  .seg-done { background: var(--done); }
  .seg-prog { background: var(--prog); }
  .seg-prio { background: var(--prio); }
  .seg-other { background: var(--other); }
  .legend { display: flex; flex-wrap: wrap; gap: 16px; margin-top: 10px; font-size: 12px; color: var(--muted); }
  .dot { display: inline-block; width: 10px; height: 10px; border-radius: 3px; margin-right: 6px; vertical-align: middle; }
  .tabs { display: flex; gap: 8px; margin: 18px 0 16px; flex-wrap: wrap; }
  .tab-btn { background: var(--panel); color: var(--muted); border: 1px solid var(--border); border-radius: 8px; padding: 8px 14px; font-size: 13px; font-weight: 600; cursor: pointer; }
  .tab-btn.active { background: #1e3a5f; color: #fff; border-color: #38bdf8; }
  .controls { display: flex; flex-wrap: wrap; gap: 10px; margin-bottom: 18px; align-items: center; }
  .controls input, .controls select { background: var(--panel); color: var(--text); border: 1px solid var(--border); border-radius: 8px; padding: 8px 12px; font-size: 13px; }
  .controls input { min-width: 260px; flex: 1; max-width: 420px; }
  .band-list { display: flex; flex-direction: column; gap: 10px; }
  .band-row { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 12px 16px; }
  .band-top { display: flex; justify-content: space-between; gap: 10px; font-size: 13px; margin-bottom: 8px; flex-wrap: wrap; }
  .subsystem-grid { display: flex; flex-direction: column; gap: 16px; }
  .sub-card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 16px 18px; }
  .sub-head { display: flex; justify-content: space-between; align-items: center; gap: 12px; margin-bottom: 10px; flex-wrap: wrap; }
  .sub-title { font-weight: 700; font-size: 15px; }
  .sub-stats { font-size: 12px; color: var(--muted); font-family: ui-monospace, Consolas, monospace; }
  .mini-bar { height: 6px; border-radius: 999px; background: var(--raw); overflow: hidden; display: flex; margin-bottom: 14px; }
  .tile-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(230px, 1fr)); gap: 9px; }
  .tile { border-radius: 9px; padding: 10px 12px; border: 1px solid var(--border); cursor: pointer; transition: transform 0.1s ease, border-color 0.15s ease; }
  .tile:hover { transform: translateY(-1px); border-color: #94a3b8; }
  .tile.done { background: var(--done-bg); border-color: rgba(34, 197, 94, 0.35); }
  .tile.in_progress { background: var(--prog-bg); border-color: rgba(245, 158, 11, 0.65); }
  .tile.queued_priority { background: var(--prio-bg); border-color: rgba(56, 189, 248, 0.32); }
  .tile.queued_other { background: var(--other-bg); border-color: rgba(100, 116, 139, 0.3); }
  .tile-top { display: flex; justify-content: space-between; align-items: center; gap: 6px; }
  .addr { font-family: ui-monospace, Consolas, monospace; font-size: 11px; color: var(--muted); }
  .badge { font-size: 10px; font-weight: 700; text-transform: uppercase; padding: 2px 6px; border-radius: 999px; letter-spacing: 0.04em; }
  .b-done { background: rgba(34,197,94,0.25); color: #86efac; }
  .b-prog { background: rgba(245,158,11,0.3); color: #fcd34d; }
  .b-prio { background: rgba(56,189,248,0.22); color: #7dd3fc; }
  .b-other { background: rgba(148,163,184,0.2); color: #cbd5e1; }
  .fn-name { font-weight: 700; font-size: 13px; margin-top: 4px; word-break: break-word; }
  .fn-what { font-size: 11.5px; color: #c7d4e6; margin-top: 4px; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
  .tag-row { display: flex; gap: 5px; flex-wrap: wrap; margin-top: 7px; }
  .tag { font-size: 10px; padding: 1px 6px; border-radius: 4px; background: rgba(255,255,255,0.07); color: #cbd5e1; }
  .tag.online { background: rgba(168,85,247,0.25); color: #d8b4fe; }
  .tag.mods { background: rgba(236,72,153,0.22); color: #f9a8d4; }
  .task-list { display: flex; flex-direction: column; gap: 14px; }
  .task-card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 16px 18px; }
  .task-card.in_progress { border-color: rgba(245, 158, 11, 0.7); }
  .task-card.done { border-color: rgba(34, 197, 94, 0.5); }
  .task-head { display: flex; justify-content: space-between; align-items: center; gap: 10px; flex-wrap: wrap; }
  .task-id { font-family: ui-monospace, Consolas, monospace; font-size: 12px; font-weight: 800; padding: 3px 8px; border-radius: 6px; background: #1e293b; margin-right: 8px; }
  .modal-backdrop { position: fixed; inset: 0; background: rgba(0,0,0,0.65); display: none; align-items: center; justify-content: center; padding: 20px; z-index: 50; }
  .modal-backdrop.open { display: flex; }
  .modal { background: var(--panel-2); border: 1px solid var(--border); border-radius: 14px; max-width: 580px; width: 100%; padding: 20px 22px; }
  .modal h2 { margin: 0 0 6px; font-size: 18px; }
  .modal-meta { font-family: ui-monospace, Consolas, monospace; font-size: 12px; color: var(--muted); margin-bottom: 12px; }
  .modal-row { margin: 8px 0; font-size: 13px; }
  .modal-row strong { color: var(--muted); display: inline-block; min-width: 115px; }
  .close-btn { margin-top: 14px; background: #263248; color: #fff; border: none; border-radius: 8px; padding: 8px 14px; cursor: pointer; font-weight: 600; }
</style>
</head>
<body>
<header>
  <div>
    <h1>EOD-Recomp Full-Game Engine Decompilation Progression Map</h1>
    <div class="subtitle">Honest tracking of ALL <code>SLUS_209.10</code> functions + Mod &amp; Online priority surface (<code>diff_run</code> / <code>EOD_DIFF</code> verified)</div>
  </div>
  <div class="timestamp" id="genAt"></div>
</header>

<main>
  <div class="kpis" id="kpis"></div>
  <div class="bar-card" id="fullBinaryBar"></div>
  <div class="bar-card" id="trackedSurfaceBar"></div>

  <div class="tabs">
    <button class="tab-btn active" data-view="fullmap">Full Binary Map (All 13,481 Functions)</button>
    <button class="tab-btn" data-view="pipeline">Mod &amp; Online Priority Pipeline</button>
    <button class="tab-btn" data-view="subsystems">Detailed Tracked Functions</button>
  </div>

  <div id="viewFullmap" class="band-list"></div>
  <div id="viewPipeline" class="task-list" style="display:none;"></div>

  <div class="controls" id="controls" style="display:none;">
    <input type="text" id="search" placeholder="Search by address (0x00189648), function name, or what it does..." />
    <select id="statusFilter">
      <option value="all">Status: All</option>
      <option value="done">Done (EOD_DIFF Verified)</option>
      <option value="in_progress">In Progress (Active Subagent)</option>
      <option value="queued_priority">Left — Mod &amp; Online Priority</option>
      <option value="queued_other">Left — Other Hooked Engine</option>
      <option value="left">All Left (Not Yet Done)</option>
    </select>
    <select id="tagFilter">
      <option value="all">Tag: All</option>
      <option value="online">Online Multiplayer</option>
      <option value="mods">Mod Focuses</option>
      <option value="physics">Physics &amp; Vehicle</option>
      <option value="weapons">Explosive Chicken</option>
      <option value="battle">Battle &amp; AI</option>
      <option value="career">Career &amp; Garage</option>
      <option value="rendering">Rendering &amp; World</option>
      <option value="ui">UI &amp; HUD</option>
      <option value="audio">Audio</option>
    </select>
  </div>
  <div id="viewSubsystems" class="subsystem-grid" style="display:none;"></div>
</main>

<div class="modal-backdrop" id="modalBackdrop"><div class="modal" id="modalBody"></div></div>

<script>
const DATA = __DATA_JSON__;
const STATUS_LABEL = {
  done: ["Done", "b-done"],
  in_progress: ["In Progress", "b-prog"],
  queued_priority: ["Queued (Mod/Online)", "b-prio"],
  queued_other: ["Queued (Engine)", "b-other"],
};

document.getElementById("genAt").textContent = `Updated: ${DATA.generated_at} · ELF Total: ${DATA.total_elf_funcs.toLocaleString()} · Named Symbols: ${DATA.symbols_count}`;

const counts = { done: 0, in_progress: 0, queued_priority: 0, queued_other: 0 };
for (const e of DATA.entries) counts[e.status] = (counts[e.status] || 0) + 1;
const trackedTotal = DATA.entries.length;
const prioTotal = DATA.tasks.reduce((a, t) => a + t.total_count, 0);
const prioDone = DATA.tasks.reduce((a, t) => a + t.done_count, 0);
const rawRemainingGame = DATA.game_elf_funcs - DATA.done_game;

document.getElementById("kpis").innerHTML = `
  <div class="kpi">
    <div class="kpi-label">True Full-Game Progress</div>
    <div class="kpi-val" style="color:var(--done)">${DATA.done_game} / ${DATA.game_elf_funcs.toLocaleString()}</div>
    <div class="kpi-sub">${((DATA.done_game / DATA.game_elf_funcs) * 100).toFixed(2)}% of game code (+ ${DATA.sdk_elf_funcs.toLocaleString()} Sony SDK/CRT)</div>
  </div>
  <div class="kpi">
    <div class="kpi-label">Game Functions Remaining</div>
    <div class="kpi-val" style="color:#f8fafc">${rawRemainingGame.toLocaleString()}</div>
    <div class="kpi-sub">Still running as static recomp C (gen/rec_*.c)</div>
  </div>
  <div class="kpi">
    <div class="kpi-label">Active Parallel Subagents</div>
    <div class="kpi-val" style="color:var(--prog)">${counts.in_progress} fns</div>
    <div class="kpi-sub">Active in priority tasks (isolated worktrees)</div>
  </div>
  <div class="kpi">
    <div class="kpi-label">Mod &amp; Online Wave 1</div>
    <div class="kpi-val" style="color:var(--prio)">${prioDone} / ${prioTotal}</div>
    <div class="kpi-sub">${prioTotal - prioDone} priority entrypoints left in Wave 1</div>
  </div>
`;

const pctGame = ((DATA.done_game / DATA.game_elf_funcs) * 100).toFixed(2);
document.getElementById("fullBinaryBar").innerHTML = `
  <div class="bar-header">
    <strong>Full Game Binary (SLUS_209.10 Game Code: ${DATA.game_elf_funcs.toLocaleString()} functions, excluding ${DATA.sdk_elf_funcs.toLocaleString()} Sony SDK/CRT)</strong>
    <span style="font-family:ui-monospace,Consolas,monospace">${DATA.done_game} decompiled (${pctGame}%) · ${rawRemainingGame.toLocaleString()} still static recomp C</span>
  </div>
  <div class="stacked-bar">
    <div class="seg-done" style="width:${Math.max(0.4, (DATA.done_game / DATA.game_elf_funcs) * 100)}%"></div>
    <div class="seg-prog" style="width:${Math.max(0.2, (counts.in_progress / DATA.game_elf_funcs) * 100)}%"></div>
    <div class="seg-prio" style="width:${Math.max(0.3, (counts.queued_priority / DATA.game_elf_funcs) * 100)}%"></div>
  </div>
`;

document.getElementById("trackedSurfaceBar").innerHTML = `
  <div class="bar-header">
    <strong>Hooked / Priority Engine Surface (${trackedTotal} functions currently mapped in HLE + Wave 1)</strong>
    <span style="font-family:ui-monospace,Consolas,monospace">${counts.done} done · ${counts.in_progress} active · ${counts.queued_priority + counts.queued_other} left (${((counts.done / trackedTotal) * 100).toFixed(1)}%)</span>
  </div>
  <div class="stacked-bar">
    <div class="seg-done" style="width:${(counts.done / trackedTotal) * 100}%"></div>
    <div class="seg-prog" style="width:${(counts.in_progress / trackedTotal) * 100}%"></div>
    <div class="seg-prio" style="width:${(counts.queued_priority / trackedTotal) * 100}%"></div>
    <div class="seg-other" style="width:${(counts.queued_other / trackedTotal) * 100}%"></div>
  </div>
  <div class="legend">
    <span><span class="dot" style="background:var(--done)"></span>Byte-Exact C++ Verified (${counts.done})</span>
    <span><span class="dot" style="background:var(--prog)"></span>In Progress (${counts.in_progress})</span>
    <span><span class="dot" style="background:var(--prio)"></span>Wave 1 Mod &amp; Online (${counts.queued_priority})</span>
    <span><span class="dot" style="background:var(--other)"></span>Other HLE Wrapped (${counts.queued_other})</span>
    <span><span class="dot" style="background:var(--raw);border:1px solid #475569"></span>Unmapped Static Recomp C (${(DATA.game_elf_funcs - trackedTotal).toLocaleString()} game fns)</span>
  </div>
`;

// Render Full Binary Address-Space Bands
document.getElementById("viewFullmap").innerHTML = DATA.bands.map(b => {
  const pct = b.total ? ((b.done / b.total) * 100).toFixed(2) : "0.00";
  return `
    <div class="band-row" style="${b.is_sdk ? 'opacity:0.75' : ''}">
      <div class="band-top">
        <strong>${b.label}</strong>
        <span class="sub-stats">${b.done} done / ${b.total.toLocaleString()} total (${pct}%) · ${b.in_progress} active · ${b.queued_priority} priority · ${b.hle_wrapped} HLE · ${b.raw_recomp.toLocaleString()} raw recomp</span>
      </div>
      <div class="stacked-bar" style="height:10px;">
        <div class="seg-done" style="width:${b.done ? Math.max(0.5, (b.done / b.total) * 100) : 0}%"></div>
        <div class="seg-prog" style="width:${b.in_progress ? Math.max(0.4, (b.in_progress / b.total) * 100) : 0}%"></div>
        <div class="seg-prio" style="width:${b.queued_priority ? Math.max(0.4, (b.queued_priority / b.total) * 100) : 0}%"></div>
        <div class="seg-other" style="width:${b.hle_wrapped ? Math.max(0.4, (b.hle_wrapped / b.total) * 100) : 0}%"></div>
      </div>
    </div>
  `;
}).join("");

function openModal(e) {
  const [lbl, cls] = STATUS_LABEL[e.status] || ["Queued", "b-other"];
  document.getElementById("modalBody").innerHTML = `
    <div style="display:flex;justify-content:space-between;align-items:center;gap:10px;">
      <h2>${e.name}</h2>
      <span class="badge ${cls}">${lbl}</span>
    </div>
    <div class="modal-meta">${e.addr} · ${e.subsystem}${e.task_id ? " · Task " + e.task_id : ""}</div>
    <div class="modal-row"><strong>What it is:</strong> ${e.what || "—"}</div>
    <div class="modal-row"><strong>Source file:</strong> <code>${e.file || "Not yet decompiled"}</code></div>
    ${e.cpp_fn ? `<div class="modal-row"><strong>C++ symbol:</strong> <code>${e.cpp_fn}</code></div>` : ""}
    ${e.hle_comment ? `<div class="modal-row"><strong>HLE note:</strong> ${e.hle_comment}</div>` : ""}
    <div class="tag-row" style="margin-top:12px;">${e.tags.map(t => `<span class="tag ${t}">${t}</span>`).join("")}</div>
    <button class="close-btn" onclick="document.getElementById('modalBackdrop').classList.remove('open')">Close</button>
  `;
  document.getElementById("modalBackdrop").classList.add("open");
}
document.getElementById("modalBackdrop").addEventListener("click", (ev) => {
  if (ev.target.id === "modalBackdrop") ev.target.classList.remove("open");
});

function renderSubsystems() {
  const q = document.getElementById("search").value.trim().toLowerCase();
  const sf = document.getElementById("statusFilter").value;
  const tf = document.getElementById("tagFilter").value;
  const filtered = DATA.entries.filter(e => {
    if (sf === "left" && e.status === "done") return false;
    if (sf !== "all" && sf !== "left" && e.status !== sf) return false;
    if (tf !== "all" && !e.tags.includes(tf)) return false;
    if (q && !(e.addr + " " + e.name + " " + e.what + " " + e.subsystem + " " + e.file).toLowerCase().includes(q)) return false;
    return true;
  });
  const groups = new Map();
  for (const e of filtered) {
    if (!groups.has(e.subsystem)) groups.set(e.subsystem, []);
    groups.get(e.subsystem).push(e);
  }
  const sortedGroups = [...groups.entries()].sort((a, b) => {
    const score = (arr) => arr.some(x => x.status === "in_progress") ? 0 : arr.some(x => x.status === "queued_priority") ? 1 : arr.some(x => x.status === "queued_other") ? 2 : 3;
    return score(a[1]) - score(b[1]) || a[0].localeCompare(b[0]);
  });
  const container = document.getElementById("viewSubsystems");
  if (!sortedGroups.length) { container.innerHTML = `<div class="sub-card" style="color:var(--muted)">No functions match.</div>`; return; }
  container.innerHTML = "";
  for (const [sub, items] of sortedGroups) {
    const d = items.filter(x => x.status === "done").length;
    const p = items.filter(x => x.status === "in_progress").length;
    const qp = items.filter(x => x.status === "queued_priority").length;
    const qo = items.filter(x => x.status === "queued_other").length;
    const n = items.length;
    const card = document.createElement("div");
    card.className = "sub-card";
    card.innerHTML = `
      <div class="sub-head"><div class="sub-title">${sub}</div><div class="sub-stats">${d}/${n} done${p ? ` · ${p} active` : ""}${qp+qo ? ` · ${qp+qo} left` : ""}</div></div>
      <div class="mini-bar"><div class="seg-done" style="width:${(d/n)*100}%"></div><div class="seg-prog" style="width:${(p/n)*100}%"></div><div class="seg-prio" style="width:${(qp/n)*100}%"></div><div class="seg-other" style="width:${(qo/n)*100}%"></div></div>
      <div class="tile-grid"></div>`;
    const grid = card.querySelector(".tile-grid");
    for (const e of items) {
      const [lbl, cls] = STATUS_LABEL[e.status] || ["Queued", "b-other"];
      const tile = document.createElement("div");
      tile.className = "tile " + e.status;
      tile.innerHTML = `<div class="tile-top"><span class="addr">${e.addr}</span><span class="badge ${cls}">${lbl}</span></div><div class="fn-name">${e.name}</div><div class="fn-what">${e.what}</div><div class="tag-row">${e.task_id ? `<span class="tag">${e.task_id}</span>` : ""}${e.tags.map(t => `<span class="tag ${t}">${t}</span>`).join("")}</div>`;
      tile.addEventListener("click", () => openModal(e));
      grid.appendChild(tile);
    }
    container.appendChild(card);
  }
}

function renderPipeline() {
  const el = document.getElementById("viewPipeline");
  el.innerHTML = DATA.tasks.map(t => {
    const [lbl, cls] = t.status === "done" ? ["Done", "b-done"] : t.status === "in_progress" ? ["In Progress", "b-prog"] : ["Queued", "b-prio"];
    return `
      <div class="task-card ${t.status}">
        <div class="task-head">
          <div><span class="task-id">${t.id}</span><strong>${t.title}</strong></div>
          <div><span class="sub-stats" style="margin-right:10px;">${t.done_count}/${t.total_count} done${t.commit ? " · " + t.commit : ""}</span><span class="badge ${cls}">${lbl}</span></div>
        </div>
        <div class="subtitle" style="margin:6px 0 12px;">${t.notes}</div>
        <div class="tile-grid">
          ${t.functions.map(f => {
            const st = f.done ? "done" : (t.status === "in_progress" ? "in_progress" : "queued_priority");
            const [fl, fc] = STATUS_LABEL[st];
            return `<div class="tile ${st}"><div class="tile-top"><span class="addr">${f.addr}</span><span class="badge ${fc}">${fl}</span></div><div class="fn-name">${f.name}</div><div class="fn-what">${f.what}</div></div>`;
          }).join("")}
        </div>
      </div>`;
  }).join("");
}

document.querySelectorAll(".tab-btn").forEach(btn => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".tab-btn").forEach(b => b.classList.remove("active"));
    btn.classList.add("active");
    const v = btn.dataset.view;
    document.getElementById("viewFullmap").style.display = v === "fullmap" ? "flex" : "none";
    document.getElementById("viewPipeline").style.display = v === "pipeline" ? "flex" : "none";
    document.getElementById("viewSubsystems").style.display = v === "subsystems" ? "flex" : "none";
    document.getElementById("controls").style.display = v === "subsystems" ? "flex" : "none";
  });
});
document.getElementById("search").addEventListener("input", renderSubsystems);
document.getElementById("statusFilter").addEventListener("change", renderSubsystems);
document.getElementById("tagFilter").addEventListener("change", renderSubsystems);
renderSubsystems();
renderPipeline();
</script>
</body>
</html>
"""


def main() -> None:
    data = build_data()
    html = HTML_TEMPLATE.replace("__DATA_JSON__", json.dumps(data, indent=2))
    OUT_HTML.write_text(html, encoding="utf-8")
    print(
        f"Wrote {OUT_HTML} | FULL ELF: {data['done_game']}/{data['game_elf_funcs']} game fns "
        f"(+{data['sdk_elf_funcs']} SDK/CRT = {data['total_elf_funcs']} total) | "
        f"Tracked surface: {len(data['entries'])}"
    )


if __name__ == "__main__":
    main()
