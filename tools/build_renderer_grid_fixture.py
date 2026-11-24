#!/usr/bin/env python3
"""Generate a local performance fixture from an extracted EOD disc.

The solo Anoka No-Rules Race gets 18 cars, and the solo Demolition Derby gets
20. Launch with EOD_MOD_DIR pointing at the generated folder and
EOD_DIRECT_START='mode=action;track=Anoka;event=No-Rules Race;car=Palo;humans=1'
or use 'Demolition Derby' as the event. Omit ai= to use the contest grid.
"""
import argparse
from pathlib import Path

from mod_package import build_archive, retail_member
from valt import compile_binary_valt, parse_binary_valt


def build(disc, output):
    disc, output = disc.resolve(), output.resolve()
    if disc == output or disc in output.parents or output in disc.parents:
        raise ValueError("Select a new fixture directory separate from the installed disc")
    archive = "DATA/RES/COMMON.RES"
    data, entry, source, _ = retail_member(disc, archive, "anoka_eves.val")
    model = parse_binary_valt(data)
    sections = {s["name"]: s for s in model["sections"]}
    race = sections["$0009$contest"]
    derby = sections["$0021$contest"]
    if (dict(race["pairs"]).get("event_type") != "normal_race"
            or dict(race["pairs"]).get("max_cars") != "14"
            or derby["parent"] != "demo_derby"
            or "max_cars" in dict(derby["pairs"])
            or dict(sections["demo_derby"]["pairs"]).get("event_type") != "kill_derby"):
        raise ValueError("Unrecognized retail Anoka contest definitions")
    race["pairs"] = [(key, "18" if key == "max_cars" else value) for key, value in race["pairs"]]
    derby["pairs"].append(("max_cars", "20"))
    packed, _ = build_archive(source, {entry["name"]: compile_binary_valt(model)})
    output.mkdir(parents=True, exist_ok=False)
    destination = output / archive
    destination.parent.mkdir(parents=True)
    destination.write_bytes(packed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="fresh local overlay directory")
    args = parser.parse_args()
    build(args.disc, args.out)
