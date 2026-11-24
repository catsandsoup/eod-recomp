#!/usr/bin/env python3
"""Builds the native renderer's material table (stage R6, docs/NATIVE_RENDERER.md "Stage R6"):
runtime/render/material_table.inc, the 2,728 material_table.json records
(docs/MATERIAL_TAGGING.md) each with the runtime texture identity (indexKey, paletteKey, width,
height: docs/CREATION_KIT_TEXTURE_CONTRACTS.md section 2) of the disc TEX it was tagged from.

usage: python -I tools/material_runtime.py <material_table.json> <textures.json> <disc root>

A record is bound to a disc TEX by the census hash (sha1 of the decoded RGBA plus "(w, h)", as
work/graphics-lookdev/scripts/build_sheets.py computed it), else by its file name within the same
owner archive and the same size. Records with no indexed (4/8-bit) TEX keep '-' keys: the
renderer loads them but cannot attach them (direct-colour TEX has no runtime key in contract v1).
"""
import hashlib
import json
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from asset_registry import analyze_tex, inventory  # noqa: E402

OUT = os.path.join(ROOT, "runtime", "render", "material_table.inc")

# Modern GPU VFX texture stem patterns (RACE.RES & track particle/sprite VFX, stage R6 A2)
RE_VFX_FIRE = re.compile(r"^(bfflame|bfface|bfedge|burst|expl\d+|firebase|firefull)$")
RE_VFX_SMOKE = re.compile(r"^(smoke|dust|dust1|haydust|trkdust|trksmoke|w_dirt)$")
RE_VFX_SPARK = re.compile(r"^(spark|fwpt|fwtrail|efx_line|camflash|flashlt)$")

# Deterministic resolution rules for uncertain ("other") & unmatched disc TEX members
AUTO_NAME_RULES = [
    (r"^sky\d", "sky", 0.0, 1.0, False),
    (r"envmap|envsky|envroad|evrmask|wenvmap|detail|ringmod|all-haul|p2ptarg", "fx", 0.0, 0.9, False),
    (r"trkdust|trksmoke|^dust\d*$|^smoke$|haydust", "vfx_smoke", 0.0, 0.9, False),
    (r"skid|tiremark|trkbsm|shad|shd|-sh\d?\.|^sh-|glare", "fake_shadow", 0.0, 1.0, False),
    (r"taxilite|^lite$|lamp|neon|flare|glow|siren", "emissive_light", 0.0, 0.5, True),
    (r"h2o|water|river|lake|pond|trkflow", "water", 0.0, 0.1, False),
    (r"tire|tyre|tread|rubber|^rub|pline2|t-lines|^cable|h-pline", "rubber", 0.0, 0.8, False),
    (r"glass|win-|window|windsh", "glass", 0.0, 0.15, False),
    (r"tree|trew|bush|brush|ivy|weed|reed|leaf|foli|plant", "foliage_cutout", 0.0, 0.85, False),
    (r"hay|grs|grass|field|lawn|turf", "grass_ground", 0.0, 0.95, False),
    (r"asph|tar|highway|croad|^road|^rd-|road\d|shings|stfinlin", "asphalt", 0.0, 0.80, False),
    (r"grav|trkgrav|stone|pebb", "gravel", 0.0, 0.90, False),
    (r"trksand|sand|beach", "sand", 0.0, 0.95, False),
    (r"mud|drt|dirt|soil|fsnd|track|trkwear|ramp|mound|path", "dirt", 0.0, 0.95, False),
    (r"rock|cliff|hill|boulder", "rock", 0.0, 0.85, False),
    (r"conc|cement|curb|kerb|sidwalk|sidewalk|sidewlk|barrier|jersey|wall|pline3", "concrete", 0.0, 0.85, False),
    (r"brick|brik", "brick", 0.0, 0.85, False),
    (r"wood|plywd|plank|board|fence|post|barn|stand|bleach|crate|ps-brom|da-glas|casket", "wood", 0.0, 0.80, False),
    (r"rust|burn", "metal_rusty", 0.4, 0.75, False),
    (r"chrome|bumper|mirror", "chrome", 0.9, 0.15, False),
    (r"fireplug|rail|guard|pipe|pole|tower|metal|steel|iron|wire|cyl|tank|barrel|drum|cage|weld|trailer|truck|bus|airstrem|tin|corrug|ma-sup|gp-tube|h-wash", "metal_painted", 0.4, 0.60, False),
    (r"sign|bill|bann|logo|ad-|poster|number|scor|font|stfn|target|arow|radar|nick|meter", "sign_decal", 0.0, 0.65, False),
    (r"crowd|people|rows|fans|spect|chick", "crowd_sprite", 0.0, 0.85, False),
    (r"driver|balon|fabric|cloth|flag", "fabric", 0.0, 0.85, False),
    (r"helmet|plastic", "plastic", 0.0, 0.45, False),
    (r"intr|seat|dash|interior", "car_interior", 0.0, 0.75, False),
    (r"chassis|bchas|body|pony|paint|livery", "car_paint", 0.1, 0.40, False),
]


def stem_of(name):
    base = name.lower().rsplit("/", 1)[-1]
    if base.endswith(".png"):
        base = base[:-4]
    return os.path.splitext(base)[0]


def classify_vfx_stem(stem, owners, surface):
    in_fx = surface in ("fx", "emissive_light") or any("race" in o for o in owners)
    if in_fx and RE_VFX_FIRE.match(stem):
        return "vfx_fire", 0.0, 0.8, True
    if (in_fx or stem in ("trkdust", "trksmoke", "dust1")) and RE_VFX_SMOKE.match(stem):
        return "vfx_smoke", 0.0, 0.9, False
    if in_fx and RE_VFX_SPARK.match(stem):
        return "vfx_spark", 0.0, 0.5, True
    return None


def resolve_record(rid, r, t):
    """Deterministically resolves uncertain (needs_human) records and classifies Modern GPU VFX."""
    rec = dict(r)
    names = t.get("names", []) if t else []
    owners = [u["owner"].lower() for u in (t.get("uses", []) if t else [])]
    stems = [stem_of(n) for n in names]

    # 1. Tag Modern GPU VFX textures (fire / smoke / sparks) explicitly from RACE.RES & fx table
    for st in stems:
        vfx = classify_vfx_stem(st, owners, rec["surface"])
        if vfx:
            rec["surface"], rec["metalness"], rec["roughness"], rec["emissive"] = vfx
            rec["needs_human"] = False
            rec["confidence"] = max(float(rec.get("confidence", 0.8)), 0.85)
            return rec, bool(r.get("needs_human"))

    if not rec.get("needs_human"):
        return rec, False

    # 2. Fix known low-confidence misclassifications & 'other' records deterministically
    if rec["surface"] == "water" and not any("afton" in o for o in owners):
        # Only Afton.trk has a real water body (docs/NATIVE_RENDERER.md); e.g. Welch f-brdg3
        rec["surface"] = "concrete"
        rec["metalness"] = 0.0
        rec["roughness"] = 0.85
        rec["emissive"] = False
    elif rec["surface"] == "other":
        matched = False
        for st in stems:
            for pat, surf, met, rough, em in AUTO_NAME_RULES:
                if re.search(pat, st):
                    rec["surface"], rec["metalness"], rec["roughness"], rec["emissive"] = surf, met, rough, em
                    matched = True
                    break
            if matched:
                break
        if not matched:
            rec["surface"] = "fx" if rec.get("alpha") == "blend" else "concrete"
            rec["metalness"] = 0.0
            rec["roughness"] = 0.85
            rec["emissive"] = False
    elif rec.get("alpha") == "blend" and rec["surface"] not in ("fx", "emissive_light", "fake_shadow", "sky", "ui"):
        # Semi-transparent blend cards (e.g. p2ptarg) stay unlit fx so they never render as solid walls
        rec["surface"] = "fx"
        rec["metalness"] = 0.0
        rec["roughness"] = 0.90

    # 3. Conservative PBR bounds on resolved low-confidence records (protect night tracks & weathered props)
    su = rec["surface"]
    if su == "metal_painted":
        rec["metalness"] = min(float(rec["metalness"]), 0.45)
        rec["roughness"] = max(float(rec["roughness"]), 0.55)
    elif su in ("metal_bare", "metal_rusty"):
        rec["roughness"] = max(float(rec["roughness"]), 0.55)
    elif su == "mixed_atlas":
        rec["metalness"] = min(float(rec["metalness"]), 0.30)
        rec["roughness"] = max(float(rec["roughness"]), 0.70)
    elif su == "car_paint":
        rec["roughness"] = max(float(rec["roughness"]), 0.35)

    rec["confidence"] = max(float(rec.get("confidence", 0.5)), 0.72)
    rec["needs_human"] = False
    return rec, True


def classify_unmatched(arch_name, tex_name):
    arch_u = arch_name.upper()
    base = tex_name.lower().rsplit("/", 1)[-1]
    st = stem_of(base)
    if arch_u.startswith("UI") or base.endswith(".art"):
        return "ui", 0.0, 0.80, False, "blend"
    vfx = classify_vfx_stem(st, [arch_name.lower()], "fx")
    if vfx:
        return vfx[0], vfx[1], vfx[2], vfx[3], "blend"
    for pat, surf, met, rough, em in AUTO_NAME_RULES:
        if re.search(pat, st):
            return surf, met, rough, em, "opaque"
    if arch_u.endswith(".CAR"):
        return "car_paint", 0.10, 0.45, False, "opaque"
    return "concrete", 0.0, 0.85, False, "opaque"


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    raw_records = json.load(open(sys.argv[1], encoding="utf-8"))["records"]
    census = json.load(open(sys.argv[2], encoding="utf-8"))["textures"]
    census_by_id = {t["id"]: t for t in census}
    by_sha = {t["sha"]: t["id"] for t in census}

    # Resolve uncertain (needs_human) records and tag Modern GPU VFX records
    records = {}
    resolved_uncertain = 0
    vfx_counts = defaultdict(int)
    for rid, r in raw_records.items():
        rec, was_uncertain = resolve_record(rid, r, census_by_id.get(rid))
        records[rid] = rec
        if was_uncertain:
            resolved_uncertain += 1
        if rec["surface"].startswith("vfx_"):
            vfx_counts[rec["surface"]] += 1

    # Stem lookup tables so Detail=maximum .tm0 textures inherit their exact .tex material record
    by_owner_stem = {}
    by_global_stem = {}
    for t in census:
        owners = {u["owner"].lower().split(".")[0] for u in t["uses"]}
        for name in t["names"]:
            st = stem_of(name)
            by_global_stem.setdefault(st, t["id"])
            for o in owners:
                by_owner_stem.setdefault((o, st), t["id"])

    _, members = inventory(sys.argv[3])
    texs = [m for m in members if m.get("type") == "TEX"]
    infos = {}

    def info_of(m):
        key = (m["path"], m["offset"])
        if key not in infos:
            with open(m["path"], "rb") as f:
                f.seek(m["offset"])
                data = f.read(m["size"])
            infos[key] = analyze_tex(data)
        return infos[key]

    keys = defaultdict(set)
    bound_ik_wh = set()
    weathered_hunter_keys = set()
    by_name = defaultdict(list)
    for m in texs:
        by_name[m["name"].lower().rsplit("/", 1)[-1]].append(m)
        info, px = info_of(m)
        if px is None:
            continue
        # Retail Hunter dealer finishes 825..829 share the same weathered body artwork.
        # Bind both resolutions by their full texture keys, as other archives reuse numbers.
        if (os.path.basename(m["archive"]).upper() == "CT.RES" and
                re.fullmatch(r"82[5-9]\.(tex|tm0)", m["name"], re.IGNORECASE)):
            weathered_hunter_keys.add((info["indexKey"], info["paletteKey"], info["width"], info["height"]))
        sha = hashlib.sha1(px + bytes(str((info["width"], info["height"])), "ascii")).hexdigest()
        rid = by_sha.get(sha)
        if rid and "indexKey" in info:
            k = (info["indexKey"], info["paletteKey"], info["width"], info["height"])
            keys[rid].add(k)
            bound_ik_wh.add((info["indexKey"], info["width"], info["height"]))
        infos[(m["path"], m["offset"])] = (info, None)  # drop the pixels

    for t in census:
        if keys.get(t["id"]):
            continue
        owners = [u["owner"].lower().split(".")[0] for u in t["uses"]]
        for name in t["names"]:
            base = name.lower()
            for cand in {base[:-4] if base.endswith(".png") else base, base.replace(".png", ".tex")}:
                for m in by_name.get(cand, []):
                    if not any(o in m["archive"].lower() for o in owners):
                        continue
                    info = info_of(m)[0]
                    if "indexKey" in info and info["width"] == t["w"] and info["height"] == t["h"]:
                        k = (info["indexKey"], info["paletteKey"], info["width"], info["height"])
                        keys[t["id"]].add(k)
                        bound_ik_wh.add((info["indexKey"], info["width"], info["height"]))

    # Pass 3 & 4: bind .tm0 high-res counterparts & remaining indexed disc TEX members
    tm0_bound = 0
    auto_records = {}
    auto_keys = defaultdict(set)
    auto_seq = 0
    for m in texs:
        info = info_of(m)[0]
        if "indexKey" not in info:
            continue
        ik_wh = (info["indexKey"], info["width"], info["height"])
        if ik_wh in bound_ik_wh:
            continue
        arch_base = os.path.basename(m["archive"])
        arch_stem = arch_base.lower().split(".")[0]
        st = stem_of(m["name"])
        rid = by_owner_stem.get((arch_stem, st)) or by_global_stem.get(st)
        k = (info["indexKey"], info["paletteKey"], info["width"], info["height"])
        if rid:
            keys[rid].add(k)
            bound_ik_wh.add(ik_wh)
            tm0_bound += 1
        else:
            surf, met, rough, em, alpha = classify_unmatched(arch_base, m["name"])
            auto_seq += 1
            aid = "A%04d" % auto_seq
            auto_records[aid] = {
                "surface": surf,
                "metalness": met,
                "roughness": rough,
                "detail": "none",
                "emissive": em,
                "alpha": alpha,
                "confidence": 0.75,
                "needs_human": False,
            }
            auto_keys[aid].add(k)
            bound_ik_wh.add(ik_wh)
            if surf.startswith("vfx_"):
                vfx_counts[surf] += 1

    all_records = dict(records)
    all_records.update(auto_records)
    for aid, ks in auto_keys.items():
        keys[aid].update(ks)

    rows, bound, still_uncertain = [], 0, 0
    for rid in sorted(all_records):
        r = all_records[rid]
        if r.get("needs_human"):
            still_uncertain += 1
        if weathered_hunter_keys.intersection(keys.get(rid, ())):
            r = dict(r, surface="metal_painted", metalness=0.0, roughness=0.85)
        fields = [r["surface"], "%.2f" % r["metalness"], "%.2f" % r["roughness"], r.get("detail") or "none",
                  "1" if r.get("emissive") else "0", r.get("alpha") or "opaque", "%.2f" % r["confidence"],
                  "1" if r.get("needs_human") else "0"]
        ks = sorted(keys.get(rid, ()))
        bound += 1 if ks else 0
        for ik, pk, w, h in ks or [("-", "-", 0, 0)]:
            rows.append("\t".join([rid, ik, pk, str(w), str(h)] + fields))
    text = ("// generated by tools/material_runtime.py from material_table.json - do not edit\n"
            "// id, indexKey, paletteKey, w, h, surface, metalness, roughness, detail, emissive, alpha,"
            " confidence, needs_human\n"
            "static const char k_material_tsv[] = R\"TSV(\n" + "\n".join(rows) + "\n)TSV\";\n")
    with open(OUT, "w", newline="\n", encoding="utf-8") as f:
        f.write(text)
    print("records", len(all_records), "(census", len(records), "+ auto", len(auto_records), ")",
          "bound", bound, "tm0/stem_added", tm0_bound, "resolved_uncertain", resolved_uncertain,
          "still_uncertain", still_uncertain, "vfx", dict(vfx_counts), "rows", len(rows), "->", OUT)


if __name__ == "__main__":
    main()
