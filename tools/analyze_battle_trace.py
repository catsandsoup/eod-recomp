#!/usr/bin/env python3
"""Summarize sampled EOD_BATTLE_TRACE output; not a gameplay acceptance oracle.

Usage: python3 tools/analyze_battle_trace.py work/battle-run [--output summary.json]
Positions are native world units. Sampled trigger transitions can miss shots.
A low-progress interval means continued throttle within a three-unit radius;
it does not identify the obstacle or prove failed pathfinding by itself.
"""
import argparse
import json
import math
import re
from collections import defaultdict
from pathlib import Path


def summarize(path):
    cars, shots = defaultdict(list), defaultdict(list)
    tracks, routes = set(), defaultdict(lambda: {"samples": 0, "course_available": 0, "routing": 0})
    phases, time, phase = set(), None, None
    car_re = re.compile(r"slot=(\d+).*pos=([^ ]+).*vel=([^ ]+).*gas=([^ ]+).*dead=(\d+) score=(-?\d+)")
    for line in path.open(errors="replace"):
        track = re.search(r"load_resource_set\(([^()]+\.trk)\) normal", line)
        if track:
            tracks.add(track[1])
        route = re.search(r"\[BATTLE_ROUTE\].*slot=(\d+) course=(\d+) routing=(\d+)", line)
        if route:
            record = routes[int(route[1])]
            record["samples"] += 1
            record["course_available"] += int(route[2])
            record["routing"] += int(route[3])
        if "[BATTLE]" in line:
            m = re.search(r"t=([\d.]+) phase=(\d+)", line)
            if m:
                time, phase = float(m[1]), int(m[2])
                phases.add(phase)
        elif "[BATTLE_CAR]" in line and time is not None:
            m = car_re.search(line)
            if m:
                cars[int(m[1])].append({"time": time, "phase": phase,
                    "position": tuple(map(float, m[2].split(","))),
                    "gas": float(m[4]), "dead": bool(int(m[5])), "score": int(m[6])})
        elif "[BATTLE_SHOT]" in line:
            m = re.search(r"slot=(\d+).*state=(\d+)", line)
            if m:
                shots[int(m[1])].append(int(m[2]))
    result = {"loaded_tracks": sorted(tracks), "phases_observed": sorted(phases),
              "route_samples": dict(sorted(routes.items())), "slots": {}}
    for slot, rows in sorted(cars.items()):
        life_start, low_start = None, None
        max_life_displacement, longest_low_progress = 0, 0
        for row in rows:
            if row["dead"] or row["phase"] != 1:
                life_start, low_start = None, None
                continue
            if life_start is None:
                life_start = row["position"]
            max_life_displacement = max(max_life_displacement, math.dist(life_start, row["position"]))
            if row["gas"] <= 0:
                low_start = None
            elif low_start is None or math.dist(low_start["position"], row["position"]) > 3:
                low_start = row
            else:
                longest_low_progress = max(longest_low_progress, row["time"] - low_start["time"])
        states = shots[slot]
        result["slots"][str(slot)] = {
            "first_time": rows[0]["time"], "last_time": rows[-1]["time"],
            "max_displacement_in_one_alive_period": round(max_life_displacement, 2),
            "sampled_fire_transitions": sum(a == 2 and b == 0 for a, b in zip(states, states[1:])),
            "observed_respawns": sum(a["dead"] and not b["dead"] for a, b in zip(rows, rows[1:])),
            "min_score": min(r["score"] for r in rows), "max_score": max(r["score"] for r in rows),
            "longest_throttled_interval_within_3_units_seconds": round(longest_low_progress, 2),
        }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    source = args.path / "out.log" if args.path.is_dir() else args.path
    output = json.dumps(summarize(source), indent=2) + "\n"
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end="")
