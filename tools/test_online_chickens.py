#!/usr/bin/env python3
"""Exercise native human and AI Battle weapons over two game processes and a rematch."""
import argparse
from collections import Counter, defaultdict
from contextlib import ExitStack
import math
import os
from pathlib import Path
import re

from test_online_completion import await_output, launch
from test_online_impact import DelayedLoopback


class ChickenLossPath(DelayedLoopback):
    """Delay and lose real datagrams, including the first weapon message in both directions."""
    def __init__(self, port):
        super().__init__(port, rtt_ms=125, guest_to_host_share=.35, jitter_ms=13, loss=.02)
        self.lost = Counter()
        self.first_request = None
        self.repeated_old_request = False

    def drop_for_stall(self, direction, packet):
        if len(packet) < 13 or packet[:3] != b'\x7d\x3e\x1e':
            return False
        event = int.from_bytes(packet[4:8], 'big')
        if direction == 0:
            if event == 1 and self.first_request is None:
                self.first_request = packet
            if event == 2 and self.first_request and not self.repeated_old_request:
                # Arrives on the same socket and route as a legitimate late request.
                self.link.sendto(self.first_request, self.host)
                self.repeated_old_request = True
        key = event, direction
        if self.lost[key] < 2:
            self.lost[key] += 1
            return True
        return False


def pad_pulses(guest):
    # A release is required to re-arm. The phase difference also lets the
    # receivers prove that shots were fired by two independent input devices.
    rows = ["400:0008", "410:0000"]
    for tick in range(570 + (3 if guest else 0), 18000, 15):
        rows.extend([f"{tick}:0100", f"{tick+6}:0000"])
    return ",".join(rows)


def event_segment(log, event):
    start = f"[netplay] event {event} 'Battle (4 AI)' starting"
    if start not in log:
        return ""
    return log.split(start, 1)[1].split(f"[netplay] event {event+1} '", 1)[0]


def wire_events(body, event):
    by_verb = defaultdict(dict)
    pattern = (rf"\[netplay-chicken\] ([\w-]+) event={event} slot=(\d+) life=(\d+) shot=(\d+) "
               r"tick=\d+ pos=([^ ]+) vel=([^\s]+)")
    for verb, slot, life, shot, point, velocity in re.findall(pattern, body):
        key = int(slot), int(life), int(shot)
        assert key not in by_verb[verb], f"event {event}: duplicate {verb} for {key}"
        by_verb[verb][key] = tuple(float(x) for x in point.split(',')), tuple(float(x) for x in velocity.split(','))
    return by_verb


def check_event(logs, event):
    host, guest = (event_segment(logs[who], event) for who in ('host', 'guest'))
    owners = dict((int(slot), int(owner)) for slot, owner in re.findall(r"bind slot=(\d+) owner=(\d+)", host))
    if len(owners) != 6 or len(re.findall(r"bind slot=(\d+) owner=(\d+)", guest)) != 6:
        return None
    assert owners == {int(s): int(o) for s, o in re.findall(r"bind slot=(\d+) owner=(\d+)", guest)}
    host_shots, guest_shots = wire_events(host, event), wire_events(guest, event)
    flight_slots = {int(slot) for slot in re.findall(r"\[BATTLE_SHOT\] slot=(\d+) [^\n]*?flight=1 ", guest)}
    sources = set()
    corroborated = []
    for key, explosion in guest_shots["replay-blast"].items():
        shooter = key[0]
        if key not in host_shots["launch"] or key not in host_shots["blast"]:
            continue  # Processes flush logs independently.
        owner = owners[shooter]
        proof = "echo-launch" if owner == 1 else "replay-launch"
        if key not in guest_shots[proof]:
            continue
        if owner == 1 and key not in guest_shots["request"]:
            continue
        assert host_shots["blast"][key] == explosion, f"event {event}: explosion moved in transit for {key}"
        assert host_shots["launch"][key] == guest_shots[proof][key], f"muzzle or velocity changed for {key}"
        if shooter not in flight_slots:
            continue  # Require a live native scene body on the receiving peer.
        sources.add(owner)
        corroborated.append(key)
    if not {0, 1, 255}.issubset(sources):
        return None
    # Victim attribution comes from the game's impulse callback on the host.
    # Never infer a hit just from two positions falling within the blast radius.
    victims = set()
    pending_hits = defaultdict(list)
    damage_directions = set()
    samples = r"\[BATTLE_CAR\] slot=(\d+) [^\n]*? damage=([\d.]+)"
    guest_damage = defaultdict(float)
    for slot, damage in re.findall(samples, guest):
        guest_damage[int(slot)] = max(guest_damage[int(slot)], float(damage))
    victim_line = r"\[chicken\] phase=victim slot=(\d+) victim=(\d+) impulse=([^ ]+) pre_damage=([\d.]+)"
    for entry in re.finditer(f"{victim_line}|{samples}", host):
        attacker, target, impulse, before, sampled_slot, after = entry.groups()
        if sampled_slot is not None:
            slot = int(sampled_slot)
            for origin, prior in pending_hits.pop(slot, []):
                if float(after) > prior + 0.00005 and guest_damage[slot] > prior + 0.00005:
                    damage_directions.add((origin, owners[slot]))
            continue
        components = tuple(float(x) for x in impulse.split(','))
        assert all(math.isfinite(x) for x in components)
        if math.hypot(*components) and int(attacker) != int(target):
            source, slot = owners[int(attacker)], int(target)
            victims.add((source, owners[slot]))
            pending_hits[slot].append((source, float(before)))
    if not {(0, 1), (1, 0)}.issubset(victims) or not any(t == 255 for _, t in victims):
        return None
    if not any(a == 255 for a, _ in victims):
        return None
    if (not {(0, 1), (1, 0)}.issubset(damage_directions)
            or not any(target == 255 for _, target in damage_directions)
            or not any(attacker == 255 for attacker, _ in damage_directions)):
        return None
    # Check the post-contact state that the owning guest actually applied. A
    # generic contact elsewhere in the event cannot satisfy a chicken hit.
    guest_slot = next(slot for slot, owner in owners.items() if owner == 1)
    victim_ticks = {int(tick) for tick in re.findall(
        rf"\[netplay-chicken\] victim event={event} slot={guest_slot} tick=(\d+)", host)}
    checkpoint = rf"revision=(\d+) slot={guest_slot} [^\n]*?damage=([\d.]+) stage=(\d+)"
    confirmed = set(re.findall(rf"\[netplay-impact\] confirmed event={event} {checkpoint}", host))
    applied = set(re.findall(rf"\[netplay-impact\] reconciled event={event} tick=\d+ {checkpoint}", guest))
    matched = [(int(revision), float(damage), int(stage)) for revision, damage, stage in confirmed & applied
               if float(damage) > 0 and any(int(revision) - dt in victim_ticks for dt in range(13))]
    if not matched:
        return None
    return corroborated, victims, damage_directions, sorted(matched)


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--impaired", action="store_true")
    args = parser.parse_args()
    exe, disc, root = (p.resolve() for p in (args.exe, args.disc, args.root))
    root.mkdir(parents=True, exist_ok=False)
    actions = root / "host-actions.txt"
    actions.touch()
    os.environ.update(EOD_NETPLAY_TEST_CHICKEN="1", EOD_NETPLAY_TEST_CHICKEN_GRID="1",
                      EOD_BATTLE_TRACE="1", EOD_NETPLAY_TRACK="Anoka", EOD_NETPLAY_TEST_EPHEMERAL_PORT="1")
    processes = {}
    try:
        with ExitStack() as resources:
            os.environ["EOD_NETPLAY_TEST_LOBBY"] = str(actions)
            launch(processes, exe, disc, root, "host", "host", "", "Battle (4 AI)", 1, pad_script=pad_pulses(False))
            pattern = r"hosting on [^:\n]+:(\d+),"
            logs = await_output(processes, lambda l: re.search(pattern, l["host"]), 50, "host did not start")
            port = int(re.search(pattern, logs["host"])[1])
            link = resources.enter_context(ChickenLossPath(port)) if args.impaired else None
            os.environ.pop("EOD_NETPLAY_TEST_LOBBY")
            route = f"join:127.0.0.1:{link.port if link else port}"
            launch(processes, exe, disc, root, "guest", route, "", "Battle (4 AI)", 1, pad_script=pad_pulses(True))
            for event in (1, 2):
                logs = await_output(processes, lambda l: check_event(l, event), 150,
                                    f"event {event} missing human or AI shots, native remote flight, or reciprocal blast hits")
                hits, victims, damage_directions, checkpoints = check_event(logs, event)
                print(f"Event {event}: {len(hits)} shots witnessed through detonation; victim directions {sorted(victims)}", flush=True)
                print(f"Damage increases visible on both clients: {sorted(damage_directions)}", flush=True)
                print(f"Guest applied host damage (revision, damage, stage): {checkpoints}", flush=True)
                with actions.open("a") as commands:
                    commands.write(f"{event} 0 end\n")
                await_output(processes, lambda l: all(f"event={event} state=lobby native_main=1" in log for log in l.values()),
                             65, "both clients must return to their shared lobby")
            assert logs["host"].count(" joined from ") == 1
            if link:
                link.print_summary()
                assert all(link.lost[e, d] == 2 for e in (1, 2) for d in (0, 1))
                assert link.repeated_old_request
    finally:
        for process, _ in processes.values():
            if process.poll() is None:
                process.terminate()
        for process, _ in processes.values():
            process.wait(timeout=10)


if __name__ == "__main__":
    run()
