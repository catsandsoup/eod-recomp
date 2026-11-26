"""Exercise online completion and rematch in separate headless game processes.

Native-lap and native-pack race independently owned cars using the retail CPU controller.
The race, derby and objective scenarios script native terminal records.
"""
import argparse
import math
from array import array
from contextlib import ExitStack
import ipaddress
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from test_online_impact import DelayedLoopback

FIRST_TICK = 360
LAST_TICK = 690


class ResultLossPath(DelayedLoopback):
    """Delay gameplay and lose the first finish reports and results for each event."""

    def __init__(self, port, withhold_terminal=False):
        super().__init__(port, rtt_ms=150, guest_to_host_share=.25, jitter_ms=8, loss=.03)
        self.discarded = {}
        self.withhold_terminal = withhold_terminal

    def drop_for_stall(self, direction, packet):
        if len(packet) < 8 or (direction, packet[2]) not in ((0, 21), (1, 23)):
            return False
        event = int.from_bytes(packet[4:8], "big")
        key = (direction, event)
        if not (self.withhold_terminal and direction == 0) and self.discarded.get(key, 0) >= 3:
            return False
        self.discarded[key] = self.discarded.get(key, 0) + 1
        return True


class BattleRespawnLossPath(ResultLossPath):
    """Withhold three SELECT requests and the first three updated spawn records."""
    def __init__(self, port):
        super().__init__(port)
        self.life_drops = {}

    def drop_for_stall(self, direction, packet):
        if len(packet) < 8:
            return False
        kind = packet[2]
        if (direction, kind) == (1, 26):
            # Up to the first spawn, each seat is three 32-bit counters and two flags.
            if not any(packet[offset] for offset in range(26, len(packet), 14)):
                return False
        elif (direction, kind) != (0, 27):
            return super().drop_for_stall(direction, packet)
        key = kind, int.from_bytes(packet[4:8], "big")
        if self.life_drops.get(key, 0) >= 3:
            return False
        self.life_drops[key] = self.life_drops.get(key, 0) + 1
        return True


def await_output(processes, condition, timeout, reason):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        logs = {name: path.read_text(errors="replace") for name, (process, path) in processes.items()}
        if condition(logs):
            return logs
        if any(process.poll() is not None for process, _ in processes.values()):
            break
        time.sleep(0.15)
    raise RuntimeError(f"{reason}. See {next(iter(processes.values()))[1].parent.parent}")


def launch(processes, exe, disc, root, name, mode, terminal, event, guests, endpoint=None, force_relay=False,
           packaged_relay=False, physical_wreck=False, check_voice=False, mutual=False, battle_respawn=False, rendered=False, rotate_track=False, rendered_host=False, pad_script=None, mod_dir=None):
    folder = root / name
    folder.mkdir()
    path = folder / "game.log"
    env = dict(
        os.environ, EOD_NETPLAY_LIFECYCLE="1", EOD_NETPLAY_AUTO=mode,
        EOD_NETPLAY_TEST_NATURAL_REMATCH="1", EOD_NETPLAY_TEST_EVENT=event,
        EOD_NETPLAY_TEST_PEERS=str(guests),
        EOD_NETPLAY_NO_INTERNET="1", EOD_NETPLAY_NO_LAN="1", EOD_NETPLAY_CAR="Phantom",
        EOD_HEADLESS="1", EOD_SKIP_MOVIES="1", EOD_MUTE="1", EOD_NETPLAY_DEBUG="1",
        EOD_MEMCARD=str(folder / "card"), EOD_APP_SUPPORT=str(folder / "support"),
        EOD_PAD_SCRIPT=pad_script if pad_script is not None else "400:0008,410:0000", EOD_LOG="-game",
    )
    if mod_dir is not None:
        env["EOD_MOD_DIR"] = str(mod_dir.resolve())
    for key in ("EOD_NETPLAY_TEST_REMATCH", "EOD_NETPLAY_TEST_TERMINAL", "EOD_NETPLAY_TEST_IMPACT", "EOD_NETPLAY_TEST_NATIVE_LAP"):
        env.pop(key, None)
    display_args = ["--headless"]
    if rendered or (rendered_host and name == "host"):
        env.pop("EOD_HEADLESS", None)
        env["EOD_NETPLAY_TEST_RENDERED"] = "1"
        # Preserve the host's packaged fullscreen and resolution settings for
        # presentation measurements. All-window tests place multiple clients on one desktop.
        display_args = ["--windowed"] if rendered else []
    elif rendered_host:
        display_args.extend(["--scale", "1"])
        env["EOD_RENDER_SCALE"] = "1"
        env.pop("EOD_PRESENT_FEEDBACK", None)
    if rotate_track and name == "host":
        env["EOD_NETPLAY_TEST_LOBBY"] = str(root / "host-actions")
    if check_voice:
        env["EOD_NETPLAY_TEST_VOICE"] = "1"
        env["EOD_NETPLAY_TEST_VOICE_PCM"] = str(folder / "received-voice.pcm")
    if physical_wreck:
        env.pop("EOD_NETPLAY_TEST_OBSERVER", None)
        env.update(EOD_NETPLAY_TEST_IMPACT="1", EOD_NETPLAY_TEST_WRECK_DRIVER="both" if mutual else "guest",
                   EOD_NETPLAY_CAR="Algar", EOD_NETPLAY_TRACK="Anoka")
        if mutual:
            env["EOD_NETPLAY_TEST_IMPACT_GATE"] = str(root / "impact-go")
        if event in ("Battle (4 AI)", "Battle (1v1, humans only)"):
            env.update(EOD_NETPLAY_TEST_BATTLE_FRAG="1", EOD_NETPLAY_TEST_IMPACT_GATE=str(root / "impact-go"))
    elif terminal is None:
        env.update(EOD_NETPLAY_TEST_NATIVE_LAP="1", EOD_NETPLAY_CAR="Moth", EOD_NETPLAY_TRACK="Anoka", EOD_AI_AUTODRIVE="0")
    else:
        env["EOD_NETPLAY_TEST_TERMINAL"] = terminal
    if battle_respawn:
        env.update(EOD_NETPLAY_TEST_BATTLE_RESPAWN="1", EOD_BATTLE_TRACE="1")
        if name == "guest":
            # Drive SELECT through the normal pad script and retail input reader.
            pulses = [f"{f}:0001,{f+5}:0000" for f in range(600, 18000, 24)]
            env["EOD_PAD_SCRIPT"] += "," + ",".join(pulses)
        else:
            env["EOD_NETPLAY_TEST_TERMINAL"] = "1000:battle-frag"
    if endpoint or packaged_relay:
        env.pop("EOD_NETPLAY_NO_INTERNET", None)
        if packaged_relay:
            env.pop("EOD_NETPLAY_RELAY", None)
        else:
            env["EOD_NETPLAY_RELAY"] = endpoint
        env.pop("EOD_NETPLAY_FORCE_RELAY", None)
        if force_relay:
            env["EOD_NETPLAY_FORCE_RELAY"] = "1"
    with path.open("w") as output:
        process = subprocess.Popen(
            [exe, *display_args, "--data", disc, "--config", folder / "eod.ini", "--frames", "18000"],
            env=env, stdout=output, stderr=output,
        )
    processes[name] = process, path


def assert_received_voice(path, frequency):
    samples = array("h")
    samples.frombytes(path.read_bytes())
    matched = 0
    for offset in range(0, len(samples) - 959, 960):
        frame = samples[offset:offset + 960]
        if max(frame) - min(frame) < 2000:
            continue
        rising = sum(a <= 0 < b for a, b in zip(frame, frame[1:]))
        if abs(rising * 50 - frequency) < 65:
            matched += 1
    assert matched >= 12, f"{path.parent.name} received only {matched} frames matching the remote {frequency} Hz tone"


def racing_passes(cars):
    """Count visible swaps in longitudinal order while a pair is racing side by side."""
    passes = []
    for left in sorted(cars):
        for right in sorted(slot for slot in cars if slot > left):
            ticks = sorted(cars[left].keys() & cars[right].keys())
            previous_side = 0
            for before, tick in zip(ticks, ticks[1:]):
                a, b = cars[left][tick], cars[right][tick]
                da = [x - old for x, old in zip(a, cars[left][before])]
                db = [x - old for x, old in zip(b, cars[right][before])]
                forward = [x + y for x, y in zip(da, db)]
                length = math.hypot(*forward)
                # Sixty ticks are one second. Both cars must be moving along
                # the same stretch of track; a spin or opposite lane is not a pass.
                if (tick - before != 60 or math.dist(a, b) > 25 or min(math.hypot(*da), math.hypot(*db)) < 10 or
                        sum(x * y for x, y in zip(da, db)) < .8 * math.hypot(*da) * math.hypot(*db)):
                    previous_side = 0
                    continue
                separation = sum((x - y) * axis for x, y, axis in zip(a, b, forward)) / length
                if abs(separation) < 2.5:
                    continue
                side = 1 if separation > 0 else -1
                if previous_side and side != previous_side:
                    passes.append((tick, left if side > 0 else right, right if side > 0 else left))
                previous_side = side
    return passes


def assert_cpu_pack(logs):
    host = logs["host"]
    expected_peers = len(logs) - 1
    assert host.count(" joined from ") == expected_peers, "a CPU racer rejoined between races"
    assert len(re.findall(r"hosting on [^\n]+Garage Code [A-Z0-9]{5}", host)) == 1, "lobby was replaced"
    sample = (r"t=(\d+) own slot (\d+) pos ([-.\d]+) [-.\d]+ ([-.\d]+) speed ([.\d]+)"
              r" steer ([-.\d]+) gas ([.\d]+) brake ([.\d]+)")
    received = r"t=(\d+) remote slot (\d+) shown ([-.\d]+) [-.\d]+ ([-.\d]+) \(newest tick ([.\d]+)\)"
    for event_id in (1, 2):
        owners, local_slots = set(), set()
        for name, log in logs.items():
            body = log.split(f"[netplay] event {event_id} 'No-Rules Race' starting", 1)[1]
            body = body.split(f"[netplay-test] native crossing event={event_id}", 1)[0]
            cpu = re.search(rf"retail CPU event={event_id} owner=(\d+) slot=(\d+) strength=4", body)
            assert cpu, f"{name} did not select native Hard AI"
            owner, local = map(int, cpu.groups())
            owners.add(owner)
            local_slots.add(local)
            assert f"GO (slot {local}, {expected_peers} puppets)" in body
            vehicle = re.search(rf"bind slot={local} owner={owner} car=([0-9a-f]+) type=50", body)[1]
            assert re.search(rf"driver switch car={vehicle} to=autopilot ra=0018e924 .*force_auto=1", body), name
            points, gas, steer, speed = {}, [], [], []
            for tick, slot, x, z, velocity, wheel, pedal, _ in re.findall(sample, body):
                assert int(slot) == local
                points[int(tick)] = (float(x), float(z))
                speed.append(float(velocity))
                steer.append(float(wheel))
                gas.append(float(pedal))
            distance = sum(math.dist(a, b) for a, b in zip(list(points.values()), list(points.values())[1:]))
            assert len(points) >= 8 and distance >= 200, f"{name} drove only {distance:.1f} m"
            assert max(speed) >= 15 and max(steer) - min(steer) > .05 and max(gas) - min(gas) > .15, name
            cars = {local: points}
            for tick, slot, x, z, source_tick in re.findall(received, body):
                if 0 < float(source_tick):
                    cars.setdefault(int(slot), {})[int(tick)] = (float(x), float(z))
            assert len(cars) == len(logs)
            for remote, positions in cars.items():
                if remote != local:
                    motion = list(positions.values())
                    assert sum(math.dist(a, b) for a, b in zip(motion, motion[1:])) > 150, (name, remote)
            print(f"Race {event_id}, {name}: retail CPU slot {local} drove {distance:.0f} m, "
                  f"peaked at {max(speed) * 3.6:.1f} km/h, and received {expected_peers} moving rivals.")
            if name == "host":
                contact = re.search(rf"racing contact event={event_id} tick=(\d+) slots=(\d+,\d+) speed_mps=([.\d]+),([.\d]+)", body)
                passes = racing_passes(cars)
                assert contact or passes, f"race {event_id} lacked a native bumper contact at speed or a close pass"
                if contact:
                    print(f"Race {event_id}, native car contact between slots {contact[2]} at "
                          f"{float(contact[3]) * 3.6:.1f} and {float(contact[4]) * 3.6:.1f} km/h.")
                for tick, ahead, behind in passes:
                    print(f"Race {event_id}, slot {ahead} passed slot {behind} on the host at tick {tick}.")
        assert len(owners) == len(local_slots) == len(logs), "several clients drove the same car"


def assert_results(logs, scenario):
    result_pattern = r"\[netplay\] result slot=(\d+) status=(\d+) rank=(\d+)"
    host = logs["host"]
    rows = re.findall(result_pattern, host)
    assert rows and len(rows) % 2 == 0, "expected standings for two events"
    for name, log in logs.items():
        assert re.findall(result_pattern, log) == rows, f"{name} reported different standings"
        assert log.count("[netplay] GO (slot ") == 2, f"{name} did not begin both events"
        for event_id in (1, 2):
            assert f"event={event_id} state=lobby native_main=1" in log, name
    for event_id in (1, 2):
        phases = [re.findall(rf"\[lifecycle\] ms=(\d+) role=\d+ event={event_id} state=results\b", log)
                  for log in logs.values()]
        assert all(len(times) == 1 for times in phases), "each peer must enter results exactly once"
        entered = [int(times[0]) for times in phases]
        committed = re.findall(rf"event {event_id} standings committed \(\d+ entries\) ms=(\d+)", host)
        assert len(committed) == 1, f"event {event_id} must have one host outcome"
        # All processes in this test share the same monotonic clock.
        delays = [(stamp - int(committed[0])) % (1 << 32) for stamp in entered]
        assert max(delays) <= 2000, f"event {event_id} took {max(delays)} ms to reach all players"
        print(f"Event {event_id}: all players entered results within {max(delays)} ms of the host outcome"
              f" ({max(delays) - min(delays)} ms between players).")
    for entries in (rows[:len(rows) // 2], rows[len(rows) // 2:]):
        states = [(int(status), int(rank)) for _, status, rank in entries]
        if scenario in ("race", "race-four"):
            ranks = 3 if scenario == "race-four" else 2
            assert states == [(1, rank) for rank in range(1, ranks + 1)] + [(2, 0)]
        elif scenario == "race-timeout":
            assert sorted(states) == [(1, 1), (2, 0), (4, 0)]
        elif scenario in ("native-lap", "native-pack"):
            assert states == [(1, rank) for rank in range(1, len(logs) + 1)]
        elif scenario == "native-wreck":
            assert states == [(5, 1), (2, 0)]
        elif scenario == "native-mutual":
            assert states == [(2, 0), (2, 0)], f"mutual wreck awarded a survivor: {states}"
        elif scenario == "objective":
            assert sorted(states) == [(2, 0), (7, 0)]
        elif scenario == "battle-scores":
            assert len(states) == 6 and states == [(8, rank) for rank in range(1, 7)]
        elif scenario in ("battle-ram", "battle-respawn"):
            assert states == [(8, 1), (8, 2)]
        else:
            assert states[0] == (5, 1)
            assert len(states) >= 3 and all(state == (2, 0) for state in states[1:])
    race_banners = {"totalled": "totalled", "last": "countdown", "host": "finished"}
    expected_banners = {"race": race_banners, "race-four": {**race_banners, "second": "finished"},
                        "race-timeout": race_banners, "native-wreck": {"guest": "totalled"},
                        "objective": {"guest": "totalled"}}
    for driver, expected_hud in expected_banners.get(scenario, {}).items():
        for event_id in (1, 2):
            phases = re.findall(rf"event={event_id} state=(\w+) native_main=(\d+) peers=\d+ hud=(\w+)", logs[driver])
            waiting = phases.index(("racing", "0", expected_hud))
            results = phases.index(("results", "0", "results"))
            lobby = phases.index(("lobby", "1", "normal"), results)
            assert waiting < results < lobby, f"{driver} skipped the transition from {expected_hud} to results to lobby"
            assert all(hud in ("normal", "results") for _, _, hud in phases[results:lobby]), driver
    if scenario in ("race", "race-four", "race-timeout"):
        assert f"tick={FIRST_TICK}" in host
        assert "action=wreck" in logs["totalled"]
        assert "[netplay] t=600 own slot" in logs["last"], "the last racer stopped before finishing"
        if scenario != "race-timeout":
            assert f"tick={LAST_TICK}" in logs["last"]
        else:
            for event_id in (1, 2):
                def host_hud_time(hud):
                    return int(re.search(rf"\[lifecycle\] ms=(\d+) role=1 event={event_id} .* hud={hud}\b", host)[1])
                elapsed = host_hud_time("results") - host_hud_time("finished")
                assert 30000 <= elapsed <= 32000, f"finish window lasted {elapsed} ms"
    elif scenario in ("native-wreck", "native-mutual"):
        assert all("terminal event=" not in log for log in logs.values()), "injected terminal record"
        for event_id in (1, 2):
            wrecks = set(re.findall(rf"contact event={event_id} .*? victim=(\d+) .*? damage_after=1\.000000 wreck=1[012]", host))
            result = rows[:2] if event_id == 1 else rows[2:]
            assert wrecks == {slot for slot, status, _ in result if status == "2"}, f"native wrecks and standings differ in event {event_id}"
            if scenario == "native-wreck":
                # The host can process several contact points in one world step.
                # Compare the guest's flash and camera pulse with the individual
                # damage increments the native solver actually applied to its car.
                hits = re.findall(rf"contact event={event_id} .*? victim=0 other=1 .*?damage_before=([\d.]+) damage_after=([\d.]+)", host)
                native_shakes = [min(.4, max(0., 4 * (float(after) - float(before) - .025))) for before, after in hits]
                feedback = re.findall(rf"feedback event={event_id} tick=(\d+) flash=([\d.]+) shake=([\d.]+)", logs["guest"])
                assert feedback and len({tick for tick, _, _ in feedback}) == len(feedback), "missing or repeated guest impact feedback"
                assert any(float(flash) > 0 for _, flash, _ in feedback), "guest did not receive the native damage flash"
                assert any(float(shake) > 0 and any(abs(float(shake) - expected) < .0001 for expected in native_shakes)
                           for _, _, shake in feedback), "guest did not receive the native collision's camera pulse"
    elif scenario in ("native-lap", "native-pack"):
        for name, log in logs.items():
            assert "terminal event=" not in log, f"{name} injected a terminal record"
            for event_id in (1, 2):
                crossings = re.findall(rf"native crossing event={event_id} slot=(\d+) tick=(\d+) lap=2 target=1 finished=1", log)
                assert len(crossings) == 1, f"{name} did not drive through the finish in event {event_id}"
    elif scenario == "battle-ram":
        scores_pattern = r"\[netplay\] battle result slot=(\d+) rank=(\d+) score=(-?\d+)"
        scores = re.findall(scores_pattern, host)
        assert len(scores) == 4 and all(re.findall(scores_pattern, log) == scores for log in logs.values())
        assert all("battle award event=" not in log and "terminal event=" not in log for log in logs.values())
        for event_id, owner, victim in ((1, 1, 1), (2, 0, 0)):
            approach = re.search(rf"battle approach event={event_id} slot=(\d+) owner={owner} points=4", host)
            wreck = re.search(rf"contact event={event_id} .*? victim={victim} other={1 - victim} "
                              r".*?damage_after=1\.000000 wreck=1[012]", host)
            assert approach and wreck, f"native collision did not wreck the opposing driver in event {event_id}"
            assert scores[2 * (event_id - 1)] == (approach[1], "1", "5"), "collision credit went to the wrong driver"
    elif scenario == "battle-respawn":
        birth = r"event=(\d+) slot=(\d+) death=(\d+) stamp=(\d+) damage=([\d.]+) pos=([^\n]+)"
        accepted = re.findall(r"\[netplay\] Battle respawn " + birth, host)
        applied = re.findall(r"\[netplay\] Battle respawn applied " + birth, logs["guest"])
        assert len(accepted) == 2 and accepted == applied, "guest did not apply exactly the two host spawns"
        for event, slot, death, _, damage, _ in accepted:
            assert death == "1" and float(damage) == 0
            assert logs["guest"].count(f"Battle SELECT event={event} slot={slot} death=1") == 1
        for name, log in logs.items():
            rounds = re.split(r"\[netplay\] event ended", log)
            for idx, run in enumerate(rounds[:2], 1):
                guest_slot = re.search(r"bind slot=(\d+) owner=1 ", run)[1]
                host_slot = re.search(r"bind slot=(\d+) owner=0 ", run)[1]
                rows = re.findall(r"\[BATTLE_CAR\] slot=(\d+) .*?dead=(\d+) score=(-?\d+)", run)
                fallen = rows.index((guest_slot, "1", "0"))
                restored = rows.index((guest_slot, "0", "0"), fallen + 1)
                assert (host_slot, "0", "1") in rows[fallen:restored+2], f"{name} lost the collision point"
                assert (host_slot, "0", "1") in rows[restored:], f"{name} changed the score on respawn"
                spawn_log = re.search(rf"Battle respawn (?:applied )?event={idx} .*? pos=([^\n]+)", run)
                start = tuple(map(float, spawn_log[1].split(",")))
                after_spawn = run[spawn_log.end():].split("[netplay-test] battle award")[0]
                samples = re.findall(rf"\[BATTLE_CAR\] slot={guest_slot} .*?pos=([^ ]+) .*? gas=([\d.]+).*? dead=0 score=0", after_spawn)
                assert samples, f"{name} did not sample the newly restored car"
                initial_error = math.dist(start, tuple(map(float, samples[0][0].split(","))))
                assert initial_error < 5, f"{name} rewound the restored car by {initial_error:.2f} m"
                driven = max((math.dist(start, tuple(map(float, pos.split(",")))) for pos, gas in samples
                              if float(gas) > .5), default=0)
                assert driven > 2, f"{name} did not see the recovered guest drive away"
                print(f"{name} recorded the recovered guest driving {driven:.2f} m from spawn in Battle {idx}.")
                assert "battle result" in run
    elif scenario == "battle-scores":
        scores = re.findall(r"\[netplay\] battle result slot=(\d+) rank=(\d+) score=(-?\d+)", host)
        assert len(scores) == 12 and re.findall(
            r"\[netplay\] battle result slot=(\d+) rank=(\d+) score=(-?\d+)", logs["guest"]) == scores
        for event_id, owner in ((1, 1), (2, 255)):
            award = re.search(rf"battle award event={event_id} slot=(\d+) owner={owner} kills=6 penalties=1 deaths=3", host)
            assert award and scores[(event_id - 1) * 6] == (award[1], "1", "5")
            assert f"battle award event={event_id}" in logs["guest"], "guest-only victory prediction was not exercised"
    elif scenario == "objective":
        assert "action=wreck" in logs["guest"] and "action=objective" in host
        assert "[netplay] t=600 own slot" in host, "the School Bus stopped after the guest wreck"
    else:
        for event_id in (1, 2):
            ai_index = host.index(f"terminal event={event_id} tick={FIRST_TICK}")
            own_index = host.index(f"terminal event={event_id} tick={LAST_TICK}")
            result_index = host.index(f"event {event_id} standings committed")
            assert ai_index < own_index < result_index, "last AI ended the derby early"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("scenario", choices=("race", "race-four", "race-timeout", "derby", "objective", "battle-scores", "battle-ram", "battle-respawn", "native-lap", "native-pack", "native-wreck", "native-mutual"))
    parser.add_argument("--guest-mod-dir", type=Path, help="composed mod profile for guest processes")
    network = parser.add_mutually_exclusive_group()
    network.add_argument("--public-garage", choices=("auto", "relay"),
                         help="use the public endpoint packaged in the game; auto leaves route selection to the game")
    network.add_argument("--garage-route", choices=("direct", "relay", "mixed", "unavailable-direct", "lost-welcome"),
                        help="host a test Garage Code service and route guests through its code flow")
    parser.add_argument("--packaged-relay-port", type=int,
                        help="serve the loopback endpoint baked into a test build and clear the runtime override")
    parser.add_argument("--rotate-track", action="store_true", help="use the host lobby to select the next course between two CPU races")
    display = parser.add_mutually_exclusive_group()
    display.add_argument("--rendered", action="store_true", help="use a real game window and the packaged renderer for every driver")
    display.add_argument("--rendered-host", action="store_true",
                         help="use the host's packaged display defaults and run the guest simulations headless at 1x")
    parser.add_argument("--check-voice", action="store_true",
                        help="send distinct synthetic voice tones and verify remote audio at each two-player mixer")
    parser.add_argument("--impaired-results", action="store_true",
                        help="use a 150 ms asymmetric path with jitter, loss, and three lost finish and result reports")
    parser.add_argument("--impaired-public-relay", metavar="IPV4:PORT",
                        help="add 180 ms asymmetric RTT, ±8 ms jitter and 2%% loss between each guest and the public relay")
    parser.add_argument("--human-battle", action="store_true", help="run the two-human online Battle roster")
    args = parser.parse_args()
    if args.human_battle and args.scenario not in ("battle-ram", "battle-respawn"):
        parser.error("--human-battle requires battle-ram or battle-respawn")
    garage = args.public_garage or args.garage_route
    packaged = bool(args.public_garage or args.packaged_relay_port)
    if args.impaired_results and garage:
        parser.error("the impaired-results fixture controls the direct UDP path")
    if args.impaired_public_relay:
        if args.public_garage != "relay":
            parser.error("--impaired-public-relay requires --public-garage relay")
        try:
            address, port_text = args.impaired_public_relay.rsplit(":", 1)
            if not ipaddress.IPv4Address(address).is_global or not 1 <= int(port_text) <= 65535:
                raise ValueError
            upstream = address, int(port_text)
        except ValueError:
            parser.error("--impaired-public-relay requires a public IPv4 address and UDP port")
    if args.packaged_relay_port is not None:
        if not 1 <= args.packaged_relay_port <= 65535:
            parser.error("the packaged port must be between 1 and 65535")
        if args.garage_route in (None, "lost-welcome"):
            parser.error("the packaged endpoint test requires a direct, relay, mixed or unavailable-direct route")
    exe, disc, root = (p.resolve() for p in (args.exe, args.disc, args.root))
    scenario = args.scenario
    if args.rotate_track and scenario != "native-pack":
        parser.error("--rotate-track requires native-pack")
    if scenario in ("race", "race-four", "race-timeout"):
        event = "No-Rules Race"
        schedule = {"host": f"{FIRST_TICK}:finish", "last": "" if scenario == "race-timeout" else f"{LAST_TICK}:finish",
                    "totalled": "300:wreck"}
        if scenario == "race-four":
            schedule["second"] = "500:finish"
    elif scenario in ("native-lap", "native-pack"):
        event = "No-Rules Race"
        schedule = {"host": None, "guest": None}
        if scenario == "native-pack":
            schedule["rival"] = None
    elif scenario in ("native-wreck", "native-mutual"):
        event = "Demolition Derby"
        schedule = {"host": None, "guest": None}
    elif scenario == "derby":
        event = "Demolition Derby"
        schedule = {"host": f"{FIRST_TICK}:ai,{LAST_TICK}:wreck", "survivor": ""}
    elif scenario in ("battle-ram", "battle-respawn"):
        event = "Battle (4 AI)"
        schedule = {"host": None, "guest": None}
    elif scenario == "battle-scores":
        event = "Battle (4 AI)"
        schedule = {"host": "690:battle-frag", "guest": "360:battle-frag"}
    else:
        event = "Detention"
        schedule = {"host": f"{LAST_TICK}:objective", "guest": f"{FIRST_TICK}:wreck"}
    if args.human_battle:
        event = "Battle (1v1, humans only)"
    root.mkdir(parents=True, exist_ok=False)
    processes = {}
    guests = len(schedule) - 1
    if args.check_voice and guests != 1:
        parser.error("checking the remote voice tones requires a two-player scenario")
    service = None
    endpoint = None
    paths = ExitStack()
    result_paths = {}
    public_paths = {}
    try:
        if args.garage_route:
            args_service = [sys.executable, str(Path(__file__).with_name("garage_relay.py")), "--bind", "127.0.0.1",
                            "--port", str(args.packaged_relay_port or 0)]
            if args.garage_route == "lost-welcome":
                gateway = Path(__file__).resolve().parents[1] / "tests/test_garage_relay.py"
                args_service = [sys.executable, "-B", str(gateway), "--drop-welcome"]
            if args.garage_route == "unavailable-direct":
                args_service.append("--relay-only")
            service = subprocess.Popen(
                args_service,
                stdout=subprocess.PIPE, text=True,
            )
            match = re.match(r"Garage relay listening on (127\.0\.0\.1:\d+)", service.stdout.readline())
            if not match:
                raise RuntimeError("Garage Code relay failed to start")
            endpoint = match[1]
        launch(processes, exe, disc, root, "host", "host", schedule["host"], event, guests, endpoint,
               packaged_relay=packaged, physical_wreck=scenario in ("native-wreck", "native-mutual", "battle-ram", "battle-respawn"), battle_respawn=scenario == "battle-respawn",
               check_voice=args.check_voice, mutual=scenario == "native-mutual", rendered=args.rendered, rotate_track=args.rotate_track, rendered_host=args.rendered_host)
        pattern = r"hosting on [^:\n]+:(\d+),"
        logs = await_output(processes, lambda logs: re.search(pattern, logs["host"]), 40, "host did not open a socket")
        port = re.search(pattern, logs["host"])[1]
        for name, terminal in schedule.items():
            if name != "host":
                mode = f"join:127.0.0.1:{port}"
                if args.impaired_results:
                    path = paths.enter_context(BattleRespawnLossPath(int(port)) if scenario == "battle-respawn" else
                                               ResultLossPath(int(port), withhold_terminal=scenario == "native-mutual"))
                    result_paths[name] = path
                    mode = f"join:127.0.0.1:{path.port}"
                if garage:
                    code = re.search(r"Garage Code ([A-Z0-9]{5})", logs["host"])[1]
                    mode = f"joincode:{code}"
                force = garage == "relay" or (args.garage_route == "mixed" and name == "totalled")
                guest_endpoint = endpoint
                if args.impaired_public_relay:
                    path = paths.enter_context(DelayedLoopback(upstream[1], 180, .25, 8, .02, relay_ip=upstream[0]))
                    public_paths[name] = path
                    guest_endpoint = f"127.0.0.1:{path.port}"
                launch(processes, exe, disc, root, name, mode, terminal, event, guests, guest_endpoint, force,
                       packaged_relay=packaged and not args.impaired_public_relay, physical_wreck=scenario in ("native-wreck", "native-mutual", "battle-ram", "battle-respawn"), battle_respawn=scenario == "battle-respawn",
                       check_voice=args.check_voice, mutual=scenario == "native-mutual", rendered=args.rendered, rotate_track=args.rotate_track, rendered_host=args.rendered_host, mod_dir=args.guest_mod_dir)
        if scenario in ("battle-ram", "battle-respawn", "native-mutual"):
            # Headless worlds may reach the same countdown tick far apart in wall time.
            # Release both approaches together so a late snapshot cannot pre-empt the collision.
            gate = root / "impact-go"
            for event_id in (1, 2):
                await_output(processes, lambda logs: all(f"[netplay-impact] ready event={event_id}" in log
                             for log in logs.values()), 90, f"cars did not reach the event {event_id} approach")
                gate.touch()
                await_output(processes, lambda logs: all(f"event={event_id} state=results" in log
                             for log in logs.values()), 100 if scenario == "battle-respawn" else 45, f"collision did not resolve event {event_id}")
                gate.unlink()
        if scenario in ("native-lap", "native-pack"):
            if args.rotate_track:
                await_output(processes, lambda logs: "event=1 state=lobby native_main=1" in logs["host"],
                             230, "host did not return to the venue selector")
                (root / "host-actions").write_text("1 0 option track 1\n")
            # Real laps depend on track speed and machine load. Give each lap
            # its own budget and still require an actual native lobby return.
            await_output(processes, lambda logs: all("event=1 state=lobby native_main=1" in log
                         for log in logs.values()), 230, "first native lap did not return to the lobby")
        logs = await_output(
            processes,
            lambda logs: all("event=2 state=lobby native_main=1" in log for log in logs.values()),
            230 if args.impaired_results or scenario in ("native-lap", "native-pack", "race-timeout", "objective") else 112,
            "peers did not reach the lobby after the second outcome",
        )
        if args.human_battle:
            for name, log in logs.items():
                for event_id in (1, 2):
                    segment = log.split(f"[netplay] event {event_id} '{event}' starting", 1)[1].split("[netplay] event ", 1)[0]
                    binds = re.findall(r"bind slot=(\d+) owner=(\d+) car=\w+ type=(\w+)", segment)
                    assert len(binds) == 2 and {owner for _, owner, _ in binds} == {"0", "1"}, (name, binds)
                    local = "0" if name == "host" else "1"
                    assert all(kind == ("50" if owner == local else "51") for _, owner, kind in binds)
        assert_results(logs, scenario)
        if scenario == "native-pack":
            assert_cpu_pack(logs)
        if args.rotate_track:
            assert "lobby seq=1 action=option" in logs["host"], "host did not change venue"
            venues = re.findall(r"online event \d+: No-Rules Race on ([^,]+),", logs["host"])
            assert len(venues) == len(set(venues)) == 2, f"host repeated the same venue: {venues}"
            for log in logs.values():
                assert re.findall(r"online event \d+: No-Rules Race on ([^,]+),", log) == venues
            print(f"The same lobby raced first at {venues[0]}, then at {venues[1]}.")
        if public_paths:
            rooms = re.findall(r"hosting on [^\n]+, Garage Code ([A-Z0-9]{5})", logs["host"])
            assert len(rooms) == 1 and logs["host"].count(" joined from relay peer ") == guests, "the party rejoined"
            print(f"Both events used Garage Code {rooms[0]} with {guests} guest admission(s).")
        for name, path in public_paths.items():
            path.print_summary()
            for event in (1, 2):
                sent, received = (direction[12, event] for direction in path.relay_events)
                results = path.relay_events[1][23, event]
                assert sent and received and results, f"{name} missing public snapshots or results for event {event}"
                print(f"{name} event {event} via {args.impaired_public_relay}: {sent} outgoing and {received} incoming "
                      f"car snapshots, {results} incoming standings packets")
        for name, path in result_paths.items():
            if path.error:
                raise path.error
            assert all(path.discarded.get((1, event)) == 3 for event in (1, 2)), name
            if scenario == "native-mutual":
                withheld = [path.discarded.get((0, event), 0) for event in (1, 2)]
                assert all(withheld), "No guest terminal reports crossed the impaired test path"
                print(f"Withheld every guest terminal report in both derbies ({withheld[0]} and {withheld[1]}).")
            if scenario == "battle-respawn":
                assert all(path.life_drops.get((kind, event)) == 3 for kind in (26, 27) for event in (1, 2))
            if name in ("totalled", "second") or (name == "last" and scenario != "race-timeout"):
                assert all(path.discarded.get((0, event)) == 3 for event in (1, 2)), name
        if args.check_voice:
            for name in logs:
                assert_received_voice(root / name / "received-voice.pcm", 665 if name == "host" else 440)
            print("Each audio mixer received the other driver's Opus tone.")
        if args.garage_route == "lost-welcome":
            assert logs["host"].count("continued through relay") == guests, "pending seats failed to resume"
            assert logs["host"].count(" joined from ") == guests, "a single guest occupied multiple seats"
        if garage:
            for name in schedule:
                if name == "host":
                    continue
                if args.public_garage == "auto":
                    selected = re.search(r"Garage Code route: Connected (directly|through the relay)", logs[name])
                    assert selected, f"{name} did not complete Garage Code route selection"
                    print(f"{name} connected {selected[1]} using the packaged public endpoint.")
                    continue
                relayed = garage in ("relay", "unavailable-direct", "lost-welcome") or (args.garage_route == "mixed" and name == "totalled")
                route = "through the relay" if relayed else "directly"
                assert f"Garage Code route: Connected {route}" in logs[name], f"{name} did not connect {route}"
        print(f"{scenario} passed: identical standings from two consecutive events on {len(logs)} peers")
    finally:
        paths.close()
        if service is not None:
            service.terminate()
            service.wait(timeout=5)
        for process, _ in processes.values():
            if process.poll() is None:
                process.terminate()
        for process, _ in processes.values():
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
