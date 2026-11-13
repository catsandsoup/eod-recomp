"""Extracts the real disc names and values used by docs/design/kit-prototype.html.

usage: python3 tools/kit_prototype_data.py DISC_DIR [docs/design/kit-prototype.html]

Writes the JSON between the KIT-DATA markers of the prototype. Only names, numbers and counts are
taken from the disc (no images, no description text), so the prototype can live in the repository.
"""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "formats"))
import modding_backend as mb  # noqa: E402
from valt import parse_binary_valt  # noqa: E402
import valt_schema  # noqa: E402

HANDLING = [
    ("Chassis", "mass"), ("Chassis", "grip"), ("Chassis", "brake_factor"), ("Chassis", "spring_factor"),
    ("Chassis", "damage_tweak"), ("EngineNormal", "power_max"), ("EngineNormal", "torque_max"),
    ("EngineNormal", "redline"), ("Transmission", "num_gears"), ("Transmission", "rear_end_ratio"),
    ("Transmission", "torque_balance"), ("Front", "tire_diameter"), ("Rear", "tire_diameter"),
]
UNUSED_SAMPLE = [("Chassis", "fuel_capacity"), ("Chassis", "wheel_lock"), ("Front", "camber"),
                 ("Front", "rate"), ("Rear", "sway"), ("Chassis", "damage_scale")]


def members(path):
    return mb.directory(path)


def table(path, name, entries=None):
    entries = entries or members(path)
    data, _ = mb.payload(path, name, entries, verify_crc=False)
    return parse_binary_valt(data)


def sections(t, name):
    return [dict(s["pairs"]) for s in t["sections"] if re.sub(r"^\$\d+\$", "", s["name"]) == name]


def first(t, name):
    found = sections(t, name)
    return found[0] if found else {}


def num(text):
    try:
        return float(str(text).split(",")[0])
    except ValueError:
        return None


def main():
    disc = Path(sys.argv[1]).expanduser()
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE.parent / "docs/design/kit-prototype.html"
    common = disc / "DATA/RES/COMMON.RES"
    ce = members(common)
    carclass = table(common, "carclass.val", ce)
    classes = {}
    for s in carclass["sections"]:
        if s["name"].startswith("$"):
            continue
        link = dict(s["pairs"]).get("@chassis", "")
        nested = next(x for x in carclass["sections"] if x["name"] == link)
        for chassis, _ in nested["pairs"]:
            classes.setdefault(chassis.lower(), s["name"])
    buy = table(common, "buycar.val", ce)
    dealer = {c["chassis"].lower(): dict(chassis=c["chassis"], price=num(c.get("price")))
              for c in sections(buy, "car") if "chassis" in c}
    variants = {}
    for v in sections(buy, "vehicle"):
        variants.setdefault(v.get("chassis", "").lower(), []).append(int(num(v.get("number")) or 0))
    stats = {c["chassis"].lower(): dict(speed=num(c.get("speed")), handling=num(c.get("handling")),
                                        durability=num(c.get("durability")))
             for c in sections(table(common, "car_stats.val", ce), "carstats") if "chassis" in c}
    cars = []
    for path in sorted((disc / "DATA/CAR").glob("*.CAR")):
        entries = members(path)
        names = [e["name"] for e in entries]
        if "physics.val" not in names:
            continue
        phys = table(path, "physics.val", entries)
        chassis_name = path.stem.split("_")[0].title()
        for key in list(dealer) + list(classes):
            if key.startswith(path.stem.split("_")[0].lower()[:6]):
                chassis_name = (dealer.get(key) or {}).get("chassis") or key.title()
                break
        handling = []
        for sec, key in HANDLING:
            values = first(phys, sec)
            if key not in values:
                continue
            spec = valt_schema.describe_field("physics.val", sec, key)
            handling.append(dict(section=sec, key=key, label=spec["label"], group=spec["group"], unit=spec["unit"],
                                 value=values[key].strip(), explanation=spec["explanation"].split(" Retail files use")[0],
                                 observed=spec.get("observed"), confidence=spec["confidence"]))
        gears = first(phys, "Transmission")
        ratios = [gears[k].strip() for k in sorted((k for k in gears if re.fullmatch(r"g\d", k)), key=lambda k: int(k[1:]))]
        unused = []
        for sec, key in UNUSED_SAMPLE:
            values = first(phys, sec)
            if key in values:
                unused.append(dict(label=valt_schema.describe_field("physics.val", sec, key)["label"], key=f"[{sec}] {key}",
                                   value=values[key].strip()))
        low = chassis_name.lower()
        cars.append(dict(archive=path.name, name=chassis_name, cls=classes.get(low, "Trailer" if "TRAILER" in path.name else "Other"),
                         price=(dealer.get(low) or {}).get("price"), variants=variants.get(low, [])[:12],
                         stats=stats.get(low), handling=handling, gears=ratios, unused=unused,
                         models=sorted(n for n in names if n.endswith(".mod"))[:12]))
    tracks_val = table(common, "tracks.val", ce)
    track_names = {s["name"].lower(): dict(s["pairs"]) for s in tracks_val["sections"]}
    tracks = []
    for path in sorted((disc / "DATA/TRACKS").glob("*.TRK")):
        entries = members(path)
        names = [e["name"] for e in entries]
        stem = path.stem.split("_")[0].lower()
        info = next((v for k, v in track_names.items() if k.startswith(stem[:6])), {})
        eves_name = f"{(info.get('name') and next(k for k, v in track_names.items() if v is info)) or stem}_eves.val"
        events = []
        if eves_name in [e["name"] for e in ce]:
            for s in table(common, eves_name, ce)["sections"]:
                c = dict(s["pairs"])
                if c.get("event_type") and not s["name"].startswith("$"):
                    events.append(dict(name=(c.get("friendly_name") or valt_schema.humanize(s["name"])).strip(),
                                       type=c["event_type"].strip(), laps=c.get("laps", "").strip(),
                                       cars=c.get("max_cars", "").strip(), layout=c.get("external_table", "").strip()))
        tracks.append(dict(archive=path.name, name=(info.get("name") or path.stem.title()).strip(),
                           longname=(info.get("longname") or "").strip(),
                           textures=sum(1 for n in names if n.endswith(".tex")),
                           sky=sum(1 for n in names if n.lower().startswith("sky") and n.endswith(".tex")),
                           lines=sorted(n for n in names if n.endswith(".ild")),
                           layouts=sorted(n for n in names if n.startswith("c_") and n.endswith(".va0")),
                           events=events[:30], shadows="shadow.val" in names))
    catalog = []
    for s in table(common, "catalog.val", ce)["sections"]:
        d = dict(s["pairs"])
        if d.get("name") and s["name"] in ("engine", "suspension", "exhaust", "aero"):
            catalog.append(dict(kind=s["name"], name=d["name"].strip(), price=num(d.get("price")),
                                fits=(d.get("car_class") or d.get("league") or "").strip()))
    data = dict(cars=cars, tracks=tracks, catalog=catalog, carPriceMultiplier=num(first(buy, "settings").get("car_price_multiplier")))
    text = out.read_text()
    blob = json.dumps(data, separators=(",", ":"))
    new = re.sub(r"(<script id=\"kit-data\" type=\"application/json\">).*?(</script>)",
                 lambda m: m.group(1) + blob + m.group(2), text, flags=re.S)
    out.write_text(new)
    print(f"{len(cars)} cars, {len(tracks)} tracks, {len(catalog)} parts -> {out}")


if __name__ == "__main__":
    main()
