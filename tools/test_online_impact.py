"""Run two or three native EOD clients through the headless Anoka impact fixture."""

import argparse
from collections import Counter
from contextlib import ExitStack
import heapq
import math
import os
from pathlib import Path
import random
import re
import selectors
import socket
import statistics
import subprocess
import threading
import time


class DelayedLoopback:
    """Delay one loopback game client on its path to a host or a fixed relay."""

    def __init__(self, host_port, rtt_ms, guest_to_host_share, jitter_ms, loss, stall_role=None, relay_ip=None):
        self.host = ("127.0.0.1", host_port)
        self.relay_ip = relay_ip
        self.uplink = None
        self.relay_events = (Counter(), Counter())
        self.delay = (rtt_ms * guest_to_host_share / 1000,
                      rtt_ms * (1 - guest_to_host_share) / 1000)
        self.jitter = jitter_ms / 1000
        self.loss = loss
        self.random = (random.Random(42), random.Random(43))
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.delays = ([], [])
        self.drops = [0, 0]
        self.error = None
        self.stall_role = stall_role
        self.client_tick = 0
        self.checkpoint_revisions = []
        self.forwarded_ack = 0
        self.stalled_packets = 0

    def drop_for_stall(self, direction, packet):
        if not self.stall_role or len(packet) < 14:
            return False
        kind = packet[2]
        if direction == 0 and kind == 12 and len(packet) >= 18:  # guest SNAP includes a checkpoint ack
            self.client_tick = max(self.client_tick, int.from_bytes(packet[14:18], "big"))
        if self.stall_role == "observer":
            # Leave heartbeats and the third driver's own inputs flowing.
            return direction == 1 and kind == 12 and 312 <= self.client_tick < 448
        if direction == 1 and kind == 25:  # K_IMPACT_STATE
            self.checkpoint_revisions.append(int.from_bytes(packet[9:13], "big"))
        # Withhold the owner's snapshots (including acknowledgements) while
        # the host resolves the impact, then let it return control normally.
        if direction == 0 and kind == 12:
            if self.checkpoint_revisions and self.client_tick < 365:
                return True
            self.forwarded_ack = max(self.forwarded_ack, int.from_bytes(packet[8:12], "big"))
        return False

    def __enter__(self):
        self.link = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.link.bind(("127.0.0.1", 0))
            self.link.setblocking(False)
            self.port = self.link.getsockname()[1]
            if self.relay_ip:
                self.uplink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.uplink.connect((self.relay_ip, self.host[1]))
                self.uplink.setblocking(False)
            self.thread.start()
        except (OSError, RuntimeError):
            self.link.close()
            if self.uplink:
                self.uplink.close()
            raise
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join()
        self.link.close()
        if self.uplink:
            self.uplink.close()

    def run(self):
        guest = None
        sequence = 0
        pending = []
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(self.link, selectors.EVENT_READ)
                if self.uplink:
                    selector.register(self.uplink, selectors.EVENT_READ)
                while not self.stop.is_set():
                    now = time.monotonic()
                    while pending and pending[0][0] <= now:
                        _, _, arrival, direction, destination, packet = heapq.heappop(pending)
                        if self.uplink and direction == 0:
                            self.uplink.send(packet)
                        else:
                            self.link.sendto(packet, destination)
                        if self.uplink and packet.startswith(b"EOR1D") and len(packet) >= 35 and packet[27:29] == b"\x7d\x3e":
                            kind, event = packet[29], int.from_bytes(packet[31:35], "big")
                            if kind in (12, 23):  # K_SNAP, K_EVENT_RESULTS
                                self.relay_events[direction][kind, event] += 1
                        self.delays[direction].append(1000 * (time.monotonic() - arrival))
                    timeout = min(0.02, max(0, pending[0][0] - now)) if pending else 0.02
                    for key, _ in selector.select(timeout):
                        packet, source = key.fileobj.recvfrom(65536)
                        arrival = time.monotonic()
                        if key.fileobj is self.uplink or (not self.uplink and source == self.host):
                            if guest is None:
                                continue
                            direction, destination = 1, guest
                        else:
                            if guest is None:
                                guest = source
                            if source != guest:
                                continue
                            direction, destination = 0, self.host
                        if self.drop_for_stall(direction, packet):
                            self.stalled_packets += 1
                            continue
                        rng = self.random[direction]
                        if rng.random() < self.loss:
                            self.drops[direction] += 1
                            continue
                        delay = max(0, self.delay[direction] + rng.uniform(-self.jitter, self.jitter))
                        sequence += 1
                        heapq.heappush(pending, (arrival + delay, sequence, arrival, direction, destination, packet))
        except OSError as error:
            self.error = error

    def print_summary(self):
        if self.error:
            raise self.error
        names = ("guest to public relay", "public relay to guest") if self.uplink else ("guest to host", "host to guest")
        for direction, name in enumerate(names):
            delays = self.delays[direction]
            if not delays:
                raise RuntimeError(f"No datagrams forwarded {name}")
            print(f"{name}: {len(delays)} forwarded, {self.drops[direction]} dropped, "
                  f"median/maximum added delay {statistics.median(delays):.2f}/{max(delays):.2f} ms")


def read_log(path):
    return path.read_text(encoding="utf-8", errors="replace")


def wait_for(process, logfile, predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = read_log(logfile)
        result = predicate(log)
        if result:
            return result
        if process.poll() is not None:
            break
        time.sleep(0.1)
    raise RuntimeError(f"Expected game state was not reached. See {logfile}.")


def start_game(args, role, mode):
    folder = args.logs / role
    folder.mkdir()
    logfile = folder / "game.log"
    env = dict(
        os.environ,
        EOD_HEADLESS="1", EOD_MUTE="1", EOD_SKIP_MOVIES="1",
        EOD_NETPLAY_AUTO=mode, EOD_NETPLAY_NO_INTERNET="1", EOD_NETPLAY_NO_LAN="1",
        EOD_NETPLAY_TEST_IMPACT="1", EOD_NETPLAY_TRACK="Anoka",
        EOD_NETPLAY_TEST_IMPACT_GATE=str(args.logs / "impact-go"),
        EOD_NETPLAY_TEST_EVENT="Demolition Derby" if args.wreck_derby else "Battle (4 AI)",
        EOD_NETPLAY_LIFECYCLE="1", EOD_NETPLAY_TEST_WRECK_DRIVER=args.wreck_driver, EOD_NETPLAY_CAR=args.host_car if role == args.impact_roles[1] else args.guest_car,
        EOD_NETPLAY_TEST_IMPACT_OFFSET=str(args.lateral_offset_m),
        EOD_NETPLAY_TEST_AIRBORNE_METRES=str(args.airborne_m),
        EOD_APP_SUPPORT=str(folder / "support"), EOD_MEMCARD=str(folder / "card"), EOD_LOG="-game",
    )
    for fixture in ("EOD_DIRECT_START", "EOD_NETPLAY_REC", "EOD_NETPLAY_PLAY",
                    "EOD_NETPLAY_TEST_REMATCH", "EOD_NETPLAY_TEST_NATURAL_REMATCH", "EOD_NETPLAY_TEST_TERMINAL",
                    "EOD_NETPLAY_TEST_FOLLOWING"):
        env.pop(fixture, None)
    if args.rendered:
        env.pop("EOD_HEADLESS", None)
        env["EOD_NETPLAY_TEST_RENDERED"] = "1"
    if args.following:
        env["EOD_NETPLAY_TEST_FOLLOWING"] = args.following
    if args.observer:
        env["EOD_NETPLAY_TEST_OBSERVER"] = "1"
        env["EOD_NETPLAY_TEST_PEERS"] = "2"
    else:
        env.pop("EOD_NETPLAY_TEST_OBSERVER", None)
    if args.pack_collision:
        env["EOD_NETPLAY_TEST_PACK_COLLISION"] = "1"
    else:
        env.pop("EOD_NETPLAY_TEST_PACK_COLLISION", None)
    if args.guest_pair:
        env["EOD_NETPLAY_TEST_GUEST_PAIR"] = "1"
    else:
        env.pop("EOD_NETPLAY_TEST_GUEST_PAIR", None)
    with logfile.open("w") as log:
        process = subprocess.Popen(
            [args.exe, "--windowed" if args.rendered else "--headless", "--data", args.disc, "--config", folder / "eod.ini", "--frames", "4200"],
            env=env, stdout=log, stderr=log,
        )
    return process, logfile


def records(log, kind):
    prefix = f"[netplay-impact] {kind} "
    return [dict(field.split("=", 1) for field in line.removeprefix(prefix).split())
            for line in log.splitlines() if line.startswith(prefix)]


def crash_camera_result(logs):
    requests = 0
    for role, log in logs.items():
        samples = [s for s in records(log, "sample") if s["slot"] == "0" and int(s["tick"]) <= 120]
        ticks = [int(s["native_tick"]) for s in samples]
        if len(ticks) != 121 or ticks != list(range(ticks[0], ticks[0] + 121)):
            return f"FAIL {role} native race clock stopped or skipped a step during the airborne collision"
        if any(int(s["paused"]) for s in samples):
            return f"FAIL {role} entered native physics pause during the airborne collision"
        requests += sum(ticks[0] <= int(t) <= ticks[-1]
                        for t in re.findall(r"crash camera skipped at native tick (\d+)", log))
    if not requests:
        return "FAIL the native incident dispatcher never requested a crash camera"
    return f"PASS {requests} native crash-camera requests skipped, 120 uninterrupted physics steps on all {len(logs)} drivers"


def owner_result(log, role, min_kmh, confirmed_damage=False):
    samples = [s for s in records(log, "sample") if s["local"] == s["slot"]]
    contacts = [c for c in records(log, "contact")
                if c["victim"] == c["local"] and (confirmed_damage or float(c["damage_after"]) > float(c["damage_before"]))]
    if not contacts:
        guards = [g for g in records(log, "guard") if g["victim"] == g["local"]]
        allowed = sum(g["allowed"] == "1" for g in guards)
        speeds = [(s["tick"], float(s["vx"]) * 3.6) for s in samples[:90]]
        rebound = "no sampled rebound"
        if len(speeds) > 1:
            before, after = max(zip(speeds, speeds[1:]), key=lambda pair: abs(pair[0][1] - pair[1][1]))
            rebound = (f"strongest sampled rebound at tick {after[0]} from {before[1]:.1f} to {after[1]:.1f} km/h")
        return (f"FAIL {role}: {rebound}. No damaging owner contact. "
                f"Native damage guard accepted {allowed}/{len(guards)} observed owner callbacks.")
    hit = contacts[0]
    tick = int(hit["tick"])
    earlier = [s for s in samples if int(s["tick"]) < tick]
    later = [s for s in samples if int(s["tick"]) == tick]
    if not earlier or not later:
        return f"FAIL {role}: native contact at tick {tick} lacks bracketing owner samples"
    before, after = earlier[-1], later[0]
    kmh = abs(float(before["vx"])) * 3.6
    recoil = abs(float(before["vx"]) - float(after["vx"])) * 3.6
    passed = kmh >= min_kmh and recoil >= 5.0
    damage = (f"host-confirmed damage" if confirmed_damage and hit["damage_before"] == hit["damage_after"]
              else f"damage {hit['damage_before']} → {hit['damage_after']}")
    return (f"{'PASS' if passed else 'FAIL'} {role}: event {hit['event']}, owner slot {hit['local']}, tick {tick}, "
            f"pre-impact {kmh:.1f} km/h, velocity changed {recoil:.1f} km/h, "
            f"{damage}, native impulse {hit['impulse']}")


def derby_result(logs, wreck_driver):
    host_wreck = wreck_driver == "host"
    hit = next((c for c in records(logs["host"], "contact")
                if (c["victim"] == c["local"]) == host_wreck and 10 <= int(c["wreck"]) <= 12), None)
    if not hit:
        return f"FAIL the native contact did not total the pre-damaged {wreck_driver}"
    rows = re.compile(r"^\[netplay\] result slot=(\d+) status=(\d+) rank=(\d+)$", re.MULTILINE)
    winner = str(1 - int(hit["victim"]))
    expected = [(winner, "5", "1"), (hit["victim"], "2", "0")]
    if any(rows.findall(logs[role]) != expected for role in ("host", "guest")):
        return "FAIL the two racers did not receive one matching survivor and totalled result"
    if not host_wreck and not any(10 <= int(r["stage"]) <= 12 for r in records(logs["guest"], "reconciled")):
        return "FAIL guest did not apply its confirmed native wreck"
    return (f"PASS native contact totalled {wreck_driver} slot {hit['victim']} at tick {hit['tick']}, "
            f"both awarded slot {winner} the win and returned to the lobby")


def host_pair_result(log, args):
    contacts = [c for c in records(log, "contact") if float(c["damage_after"]) > float(c["damage_before"])]
    if not contacts:
        return "FAIL host observed no damaging collision"
    tick = contacts[0]["tick"]
    approach = {row["slot"]: row for row in records(log, "before") if row["tick"] == tick}
    after = {row["slot"]: row for row in records(log, "sample") if row["tick"] == tick}
    if set(approach) != {"0", "1"} or set(after) != {"0", "1"}:
        return "FAIL missing host body samples around the first collision"
    before_v = [float(approach[s]["vx"]) for s in ("0", "1")]
    recoil = [(before_v[0] - float(after["0"]["vx"])) * 3.6,
              (float(after["1"]["vx"]) - before_v[1]) * 3.6]
    passed = before_v[0] > 0 > before_v[1] and min(recoil) > 5
    masses = {name: float(mass) for name, mass in re.findall(r"^\[core\] car (.+?): mass (\d+),", log, re.MULTILINE)}
    if any(name not in masses for name in (args.host_car, args.guest_car)):
        return "FAIL the host did not build the requested pair of vehicles"
    slot_mass = [masses[args.guest_car], masses[args.host_car]]
    # Component of momentum change along the approach. Mass units cancel
    # in the relative error, and unequal models can have different recoil.
    momentum = [mass * delta_v for mass, delta_v in zip(slot_mass, recoil)]
    difference = abs(momentum[0] - momentum[1]) / max(1., *momentum)
    lateral = abs(float(approach["0"]["z"]) - float(approach["1"]["z"]))
    if args.max_impulse_difference is not None:
        passed &= difference <= args.max_impulse_difference
    return (f"{'PASS' if passed else 'FAIL'} authoritative pair at tick {tick}, lateral offset {lateral:.2f} m, "
            f"recoil {recoil[0]:.1f}/{recoil[1]:.1f} km/h, longitudinal momentum-change difference {difference:.1%}")


def pack_result(logs, min_kmh):
    host = logs["host"]
    contacts = [r for r in records(host, "contact") if float(r["damage_after"]) > float(r["damage_before"])]
    if not contacts:
        return "FAIL host recorded no damaging contacts in the pileup"
    first = min(int(row["tick"]) for row in contacts)
    window = [row for row in contacts if int(row["tick"]) == first]
    pairs = {tuple(sorted((c["victim"], c["other"]))) for c in window}
    touched = {s for pair in pairs for s in pair}
    if len(pairs) != 3 or touched != {"0", "1", "2"}:
        return f"FAIL all three pairs must collide in one native step, saw {sorted(pairs)}"
    worst_error = 0
    for a, b in pairs:
        ab = sum(float(c["impulse"]) for c in window if c["victim"] == a and c["other"] == b)
        ba = sum(float(c["impulse"]) for c in window if c["victim"] == b and c["other"] == a)
        worst_error = max(worst_error, abs(ab - ba) / max(1., ab, ba))
    if worst_error > .045:
        return f"FAIL reciprocal impulses differed by {worst_error:.2%} on the same step"
    samples = {(int(s["tick"]), s["slot"]): s for s in records(host, "sample")}
    approaches = {s["slot"]: s for s in records(host, "before") if int(s["tick"]) == first}
    changes = []
    for slot in sorted(touched):
        before, after = approaches[slot], samples[first + 2, slot]
        speed = math.hypot(float(before["vx"]), float(before["vz"])) * 3.6
        recoil = math.hypot(float(after["vx"]) - float(before["vx"]), float(after["vz"]) - float(before["vz"])) * 3.6
        if speed < min_kmh or recoil < 5 or float(after["damage"]) <= 0:
            return f"FAIL slot {slot} approached at {speed:.1f} km/h, changed velocity by {recoil:.1f} km/h, damage {after['damage']}"
        changes.append(f"{speed:.1f}/{recoil:.1f}")
    return (f"PASS native three-car pileup at step {first}, pairs {sorted(pairs)}, reciprocal impulse difference "
            f"{worst_error:.3%}, approach/recoil km/h {', '.join(changes)}")


def checkpoint_result(host, guest, min_kmh):
    # A fast host can resolve the collision before the guest predicts it. In
    # that case the guest's actual native body must take the host's checkpoint.
    def key(row):
        return row["event"], row["slot"], row["revision"]

    confirmed = {key(row): row for row in records(host, "confirmed")}
    received = records(guest, "reconciled")
    if not received:
        return "FAIL no host impact reached the guest", False
    revision = 0
    measured_recoil = False
    for row in received:
        source = confirmed.get(key(row))
        if int(row["revision"]) <= revision or source is None:
            return "FAIL stale or unconfirmed impact applied", False
        revision = int(row["revision"])
        if any(row[field] != source[field] for field in ("vx", "vz", "damage", "stage")):
            return "FAIL guest native body differs from its host checkpoint", False
        before, after = float(row["before_vx"]), float(row["vx"])
        if abs(before) * 3.6 >= min_kmh and abs(before - after) * 3.6 >= 5 and float(row["damage"]) > 0:
            measured_recoil = True
    return f"PASS guest native body matched {len(received)} ordered host impact checkpoints", measured_recoil


def print_damage_comparison(logs, max_delta, impact_roles, tick=120):
    print(f"Owner and remote copy damage at fixture tick {tick}:")
    passed = True
    samples = {(role, s["slot"]): s for role, log in logs.items()
               for s in records(log, "sample") if int(s["tick"]) == tick}
    for index, owner in enumerate(impact_roles):
        slot = str(index)
        own = samples.get((owner, slot))
        for role in logs:
            if role == owner:
                continue
            remote = samples.get((role, slot))
            if own is None or remote is None:
                print(f"  Slot {slot}: missing {owner} or {role} sample")
                passed = False
                continue
            delta = abs(float(own["damage"]) - float(remote["damage"]))
            print(f"  Slot {slot}: owner {owner} {own['damage']} (state {own['wreck']}), "
                  f"remote copy on {role} {remote['damage']} (state {remote['wreck']}), delta {delta:.6f}")
            if max_delta is not None and (delta > max_delta or own["wreck"] != remote["wreck"]):
                passed = False
    if max_delta is not None:
        print(f"{'PASS' if passed else 'FAIL'} damage delta tolerance {max_delta:g}")
    return max_delta is None or passed


def following_result(logs, scenario):
    samples = {}
    for role, log in logs.items():
        samples[role] = {(int(s["tick"]), int(s["slot"])): s for s in records(log, "sample")}
        for slot in (0, 1):
            if any((tick, slot) not in samples[role] for tick in range(121)):
                return f"FAIL {role} lacks native samples for car {slot}"
    if scenario == "follow":
        gaps = []
        for role, log in logs.items():
            if records(log, "contact") or records(log, "reconciled") or records(log, "confirmed"):
                return f"FAIL {role} reported car contact while following at equal speed"
            for slot in (0, 1):
                readings = [samples[role][tick, slot] for tick in range(121)]
                if any(float(s["damage"]) > 0 or int(s["wreck"]) != 0 for s in readings):
                    return f"FAIL {role} registered damage while following at equal speed"
                distance = float(readings[-1]["x"]) - float(readings[0]["x"])
                if distance < 8:
                    return f"FAIL {role} car {slot} did not keep moving ({distance:.2f} m)"
            gaps += [float(samples[role][tick, 1]["x"]) - float(samples[role][tick, 0]["x"])
                     for tick in range(121)]
        if min(gaps) <= 3.5 or max(gaps) > 10:
            return f"FAIL cars did not stay in formation on both machines: {min(gaps):.2f}–{max(gaps):.2f} m"
        return f"PASS closed formation for 120 steps: centre spacing {min(gaps):.2f}–{max(gaps):.2f} m, zero hits and zero damage on both machines"

    contacts = records(logs["host"], "contact")
    before = {(int(s["tick"]), int(s["slot"])): float(s["vx"]) for s in records(logs["host"], "before")}
    # In each collision, the native solver should slow the trailing car and
    # accelerate the leading car on the same step. Soft pushes may never
    # invoke the retail damage callback.
    for tick in range(121):
        rear, lead = before[tick, 0], before[tick, 1]
        closing = (rear - lead) * 3.6
        lost = (rear - float(samples["host"][tick, 0]["vx"])) * 3.6
        gained = (float(samples["host"][tick, 1]["vx"]) - lead) * 3.6
        if rear > lead > 0 and min(lost, gained) > .5 and (scenario != "ram" or closing >= 40):
            break
    else:
        return "FAIL host did not resolve reciprocal motion for a moving rear-end collision"
    for role, log in logs.items():
        if role != "host":
            result, _ = checkpoint_result(logs["host"], log, 0)
            if not result.startswith("PASS"):
                return f"{result} ({role})"
    if scenario == "nudge" and any(int(samples["host"][120, slot]["wreck"]) >= 10 for slot in (0, 1)):
        return "FAIL the low-speed bumper nudge totalled a healthy car"
    if scenario == "ram" and not any(float(c["damage_after"]) > float(c["damage_before"]) for c in contacts):
        return "FAIL the high-speed rear-end collision did no damage"
    return f"PASS rear contact at native step {tick}: closing speed {closing:.1f} km/h, follower lost {lost:.1f}, lead gained {gained:.1f} km/h; the owners applied the host's checkpoints"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("logs", type=Path)
    parser.add_argument("--following", choices=("follow", "nudge", "ram"),
                        help="two cars traveling in the same direction with equal speed or a faster trailing car")
    parser.add_argument("--rendered", action="store_true", help="exercise the packaged renderer in real game windows")
    parser.add_argument("--min-impact-kmh", type=float, default=95.0,
                        help="minimum measured speed just before native contact")
    parser.add_argument("--wreck-derby", action="store_true",
                        help="start a car with 94 percent prior damage and require native wreck, standings and return")
    parser.add_argument("--wreck-driver", choices=("host", "guest", "guest2"))
    parser.add_argument("--guest-pair", action="store_true",
                        help="collide two guests while the host drives independently")
    parser.add_argument("--pack-collision", action="store_true",
                        help="drive all three cars into the same point and require native three-way contact and damage agreement")
    parser.add_argument("--observer", action="store_true",
                        help="add a third human driver and compare both contact participants on its machine")
    parser.add_argument("--host-car", default="Algar", help="second impacting car, driven by the host or second guest")
    parser.add_argument("--guest-car", default="Algar", help="first guest's impacting car")
    parser.add_argument("--airborne-m", type=float, default=0,
                        help="lift the two approaching cars this far above their normal starting height")
    parser.add_argument("--check-crash-camera", action="store_true",
                        help="require a natural crash-camera request and uninterrupted physics on every participating game")
    parser.add_argument("--snapshot-stall", action="store_true",
                        help="interrupt the airborne observer's snapshots and delay the impact owner's acknowledgement")
    parser.add_argument("--lateral-offset-m", type=float, default=0,
                        help="lateral separation between the two approach lines, 0–1.5 metres")
    parser.add_argument("--max-impulse-difference", "--max-recoil-difference", type=float,
                        help="relative tolerance for longitudinal momentum changes in the authoritative pair")
    parser.add_argument("--rtt-ms", type=float,
                        help="route through a loopback proxy that adds this round-trip delay")
    parser.add_argument("--guest-to-host-share", type=float, default=0.5,
                        help="fraction of proxy round-trip delay applied from guest to host")
    parser.add_argument("--jitter-ms", type=float, default=0,
                        help="maximum deviation in either direction from the proxy's configured one-way delay")
    parser.add_argument("--loss", type=float, default=0,
                        help="fraction of datagrams the proxy should drop in either direction")
    parser.add_argument("--max-damage-delta", type=float,
                        help="require matching native damage stages and a scalar damage difference within this tolerance")
    args = parser.parse_args()
    if args.following and (args.wreck_derby or args.observer or args.pack_collision or
                           args.snapshot_stall or args.airborne_m or args.lateral_offset_m):
        parser.error("following uses two healthy cars on the same line on the ground")
    if args.pack_collision and (args.guest_pair or args.snapshot_stall or args.check_crash_camera or args.wreck_derby
                                or args.airborne_m or args.lateral_offset_m or args.max_impulse_difference is not None):
        parser.error("the ground-level pileup uses three healthy cars without a separate observer or paired-impulse metric")
    args.observer = args.observer or args.guest_pair or args.pack_collision
    args.observing_role = "host" if args.guest_pair else "observer"
    args.impact_roles = ("guest", "guest2" if args.guest_pair else "host")
    args.wreck_driver = args.wreck_driver or ("guest" if args.guest_pair else "host")
    if args.snapshot_stall and (not args.observer or args.guest_pair or args.airborne_m != 8 or args.rtt_ms != 0):
        parser.error("the snapshot stall fixture uses --observer --airborne-m 8 --rtt-ms 0")
    end_tick = 180 if args.snapshot_stall else 120
    if args.wreck_driver not in args.impact_roles:
        parser.error("the wrecked driver must be one of the two colliding drivers")
    if not 0 <= args.airborne_m <= 12:
        parser.error("airborne height must be between zero and 12 metres")
    if args.check_crash_camera and (not args.airborne_m or (args.wreck_derby and not args.observer)):
        parser.error("the crash camera check needs a raised impact and an event that continues through tick 120")
    if not 0 <= args.lateral_offset_m <= 1.5:
        parser.error("lateral offset must be between zero and 1.5 metres")
    if args.max_impulse_difference is not None and not 0 <= args.max_impulse_difference <= 1:
        parser.error("impulse tolerance must be a fraction from zero to one")
    numeric = (args.min_impact_kmh, args.rtt_ms, args.jitter_ms, args.max_damage_delta)
    if any(value is not None and (not math.isfinite(value) or value < 0) for value in numeric):
        parser.error("speed, delay, jitter and damage tolerance must be finite and nonnegative")
    if not 0 <= args.guest_to_host_share <= 1 or not 0 <= args.loss < 1:
        parser.error("delay share must be between 0 and 1, and loss must be at least 0 and below 1")
    if args.rtt_ms is None and (args.jitter_ms or args.loss or args.guest_to_host_share != 0.5):
        parser.error("proxy delay share, jitter and loss require --rtt-ms (which may be zero)")
    args.exe, args.disc, args.logs = args.exe.resolve(), args.disc.resolve(), args.logs.resolve()
    args.logs.mkdir(parents=True, exist_ok=False)
    games = []
    try:
        host, host_log = start_game(args, "host", "host")
        games.append(host)
        address = wait_for(host, host_log, lambda log: re.search(r"hosting on [^:]+:(\d+),", log), 35)
        port = int(address[1])
        if port == 27015:
            raise RuntimeError("The diagnostic fixture must use an ephemeral port.")
        participants = {"host": (host, host_log)}
        routes = []
        with ExitStack() as stack:
            roles = ("guest", "guest2" if args.guest_pair else "observer") if args.observer else ("guest",)
            for role in roles:
                guest_port = port
                if args.rtt_ms is not None:
                    proxy = stack.enter_context(DelayedLoopback(port, args.rtt_ms, args.guest_to_host_share,
                                                               args.jitter_ms, args.loss, role if args.snapshot_stall else None))
                    routes.append(proxy)
                    guest_port = proxy.port
                game, log = start_game(args, role, f"join:127.0.0.1:{guest_port}")
                games.append(game)
                participants[role] = game, log
                if args.observer and role == "guest":
                    wait_for(host, host_log, lambda text: "as player 1" in text, 45)
            for game, log in participants.values():
                wait_for(game, log, lambda text: "[netplay-impact] ready event=1" in text, 90)
            (args.logs / "impact-go").touch()
            for game, log in participants.values():
                if args.wreck_derby and not args.observer:
                    wait_for(game, log, lambda text: "event=1 state=lobby native_main=1" in text, 110)
                else:
                    wait_for(game, log, lambda text: re.search(rf"\[netplay-impact\] sample event=\d+ tick={end_tick} .* slot=1 ", text), 90)
        for proxy in routes:
            print(f"Configured {args.rtt_ms:g} ms RTT, ±{args.jitter_ms:g} ms one-way jitter, {args.loss:.1%} loss")
            proxy.print_summary()
        logs = {role: read_log(log) for role, (_, log) in participants.items()}
        if args.following:
            result = following_result(logs, args.following)
            print(result)
            damage_passed = print_damage_comparison(logs, args.max_damage_delta or 0, args.impact_roles, end_tick)
            return 0 if result.startswith("PASS") and damage_passed else 1
        if args.pack_collision:
            results = [pack_result(logs, args.min_impact_kmh)]
            for role in ("guest", "observer"):
                result, _ = checkpoint_result(logs["host"], logs[role], args.min_impact_kmh)
                results.append(f"{result} ({role})")
            print(*results, sep="\n")
            damage_passed = print_damage_comparison(logs, args.max_damage_delta or 0, ("guest", "host", "observer"), end_tick)
            return 0 if damage_passed and all(line.startswith("PASS") for line in results) else 1
        if not (args.wreck_derby or args.airborne_m or args.guest_pair or args.observer or args.lateral_offset_m):
            # The stock head-on impact must change both cars' actual renderable damage geometry.
            # Matching health or a single detached-panel mask can conceal different dent shapes.
            for slot in (0, 1):
                final_shapes = set()
                for role, log in logs.items():
                    pattern = rf"\[netplay-impact\] shape event=1 tick=(0|120) slot={slot} bytes=([1-9]\d*) hash=([0-9a-f]+)"
                    shapes = {int(tick): (size, digest) for tick, size, digest in re.findall(pattern, log)}
                    assert 0 in shapes and 120 in shapes and shapes[0] != shapes[120], f"{role} car {slot} did not deform"
                    final_shapes.add(shapes[120])
                assert len(final_shapes) == 1, f"peers disagree on damage geometry of car {slot}"
            print("PASS both cars deformed and both machines converged on their full damage shapes")
        results = [host_pair_result(logs["host"], args)]
        for owner in args.impact_roles:
            log = logs[owner]
            confirmed = took_recoil = False
            if owner != "host":
                checkpoint, took_recoil = checkpoint_result(logs["host"], log, args.min_impact_kmh)
                results.append(checkpoint + f" ({owner})")
                confirmed = checkpoint.startswith("PASS") and any(float(row["damage"]) > 0
                                    for row in records(log, "reconciled"))
            result = owner_result(log, owner, args.min_impact_kmh, confirmed)
            if not result.startswith("PASS") and took_recoil:
                result = f"PASS {owner} took the host-confirmed damaging recoil at the required approach speed"
            results.append(result)
        if args.observer:
            samples = records(logs[args.observing_role], "observer")
            retained = [s for s in samples if 4 <= int(s["tick"]) <= 119]
            moved = math.hypot(float(retained[-1]["x"]) - float(retained[0]["x"]),
                               float(retained[-1]["z"]) - float(retained[0]["z"])) if retained else 0
            hit_tick = min((int(c["tick"]) for c in records(logs["host"], "contact")
                            if float(c["damage_after"]) > float(c["damage_before"])), default=0)
            before = max((float(s["gas"]) for s in retained if int(s["tick"]) < hit_tick), default=0)
            after = max((float(s["gas"]) for s in retained if int(s["tick"]) > hit_tick + 12), default=0)
            consecutive = [int(s["tick"]) for s in retained] == list(range(4, 120))
            passed = consecutive and moved >= 1 and min(before, after) > .25
            results.append(f"{'PASS' if passed else 'FAIL'} third driver covered {moved:.2f} m, "
                           f"throttle before/after contact {before:.2f}/{after:.2f}, "
                           f"sampled {len(retained)}/116 steps in order={consecutive}")
        if args.check_crash_camera:
            results.append(crash_camera_result(logs))
        if args.snapshot_stall:
            # Select the host-driven body on the third driver's machine.
            samples = {int(s["tick"]): s for s in records(logs["observer"], "sample") if s["slot"] == "1"}
            host = {int(s["tick"]): s for s in records(logs["host"], "sample") if s["slot"] == "1"}
            high, descending = (float(samples[t]["y"]) for t in (35, 90))
            # A solid body can rebound after landing. Measure the low point
            # and the following arrest of its fall, while the link is still cut.
            floor_tick = min(range(90, 135), key=lambda t: float(samples[t]["y"]))
            floor = float(samples[floor_tick]["y"])
            later = float(samples[145]["y"])
            error = math.dist([float(samples[end_tick][k]) for k in ("x", "y", "z")],
                              [float(host[end_tick][k]) for k in ("x", "y", "z")])
            guest_route, observer_route = routes
            retries = len(guest_route.checkpoint_revisions) - len(set(guest_route.checkpoint_revisions))
            passed = (high > 5 and descending < high - 2 and -.1 < floor < 1.1 and later > floor - .15
                      and error < 1.5 and retries > 1 and guest_route.forwarded_ack == max(guest_route.checkpoint_revisions)
                      and min(r.stalled_packets for r in routes) > 10)
            results.append(f"{'PASS' if passed else 'FAIL'} observer descended from {high:.2f} to {floor:.2f} m "
                           f"at step {floor_tick} and reached {later:.2f} m before snapshots resumed, recovered position error "
                           f"{error:.2f} m, {observer_route.stalled_packets} snapshots withheld, "
                           f"{retries} repeated impact checkpoints during {guest_route.stalled_packets} withheld owner snapshots")
        print(*results, sep="\n")
        if args.wreck_derby and args.observer:
            victim = str(args.impact_roles.index(args.wreck_driver))
            wreck = any(c["victim"] == victim and 10 <= int(c["wreck"]) <= 12
                        for c in records(logs["host"], "contact"))
            premature_results = any("[netplay] result slot=" in log for log in logs.values())
            result = "PASS" if wreck and not premature_results else "FAIL"
            print(f"{result} one native wreck left the other two human cars racing")
            results.append(result)
        elif args.wreck_derby:
            outcome = derby_result(logs, args.wreck_driver)
            print(outcome)
            return 0 if outcome.startswith("PASS") and all(line.startswith("PASS") for line in results) else 1
        damage_passed = print_damage_comparison(logs, args.max_damage_delta, args.impact_roles, end_tick)
        print(f"Logs: {args.logs}")
        return 0 if damage_passed and all(line.startswith("PASS") for line in results) else 1
    finally:
        for game in games:
            if game.poll() is None:
                game.terminate()
            try:
                game.wait(timeout=3)
            except subprocess.TimeoutExpired:
                game.kill()
                game.wait()


if __name__ == "__main__":
    raise SystemExit(main())
