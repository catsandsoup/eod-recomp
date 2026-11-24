#!/usr/bin/env python3
"""Drive the lobby's gamertag switch across two native games and a rematch."""
import argparse
import os
from pathlib import Path
import re

from test_online_completion import await_output, launch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("mode", choices=("race", "derby"))
    args = parser.parse_args()
    exe, disc, root = (p.resolve() for p in (args.exe, args.disc, args.root))
    root.mkdir(parents=True)
    is_race = args.mode == "race"
    event = "No-Rules Race" if is_race else "Demolition Derby"
    os.environ.update(EOD_NETPLAY_TEST_NAMES="1", EOD_NETPLAY_TRACK="Anoka")
    host_script = root / "host-actions.txt"
    guest_script = root / "guest-actions.txt"
    host_script.write_text("1 0 nickname Host Driver\n")
    guest_script.write_text("1 0 nickname Guest Driver\n2 0 option driver_names 1\n")
    processes = {}

    def check_names(logs, event_id):
        visible_names = set()
        for peer, log in logs.items():
            samples = re.findall(
                rf"\[netplay-names\] event={event_id} tick=\d+ owner=(\d+) name=(.*?) screen=(\d) "
                r"visible=(\d) x=([-\d.]+) y=([-\d.]+) distance=([-\d.]+)", log,
            )
            geometry = re.search(rf"\[netplay-names\] event={event_id} terrain=(\d+) boxes=(\d+)", log)
            if not geometry or not all(int(count) > 0 for count in geometry.groups()):
                return False
            local_owner = "0" if peer == "host" else "1"
            assert all(owner != local_owner for owner, *_ in samples), f"{peer} showed its own roof label"
            if not samples:
                return False
            visible_on_peer = set()
            for owner, name, screen, shown, x, y, distance in samples:
                assert name == {"0": "Host Driver", "1": "Guest Driver"}[owner], peer
                if shown == "1":
                    assert screen == "1" and 0 <= float(x) <= 1 and 0 <= float(y) <= 1
                    assert 0 < float(distance) <= 150
                    visible_on_peer.add(name)
            if not visible_on_peer:
                return False
            visible_names.update(visible_on_peer)
        return visible_names == {"Host Driver", "Guest Driver"}

    try:
        os.environ["EOD_NETPLAY_TEST_LOBBY"] = str(host_script)
        launch(processes, exe, disc, root, "host", "host", "", event, 1)
        pattern = r"hosting on [^:\n]+:(\d+),"
        logs = await_output(processes, lambda l: re.search(pattern, l["host"]), 60, "host did not open its socket")
        port = re.search(pattern, logs["host"])[1]
        os.environ["EOD_NETPLAY_TEST_LOBBY"] = str(guest_script)
        launch(processes, exe, disc, root, "guest", f"join:127.0.0.1:{port}", "", event, 1)
        for event_id, enabled, rules in ((1, is_race, 0), (2, not is_race, 32 if is_race else 64)):
            def active(logs):
                return all(log.count("GO (slot ") >= event_id and
                           re.search(rf"event {event_id} '.*?' starting .*?rules={rules}\b", log) for log in logs.values())

            logs = await_output(processes, active, 120, f"peers did not start event {event_id} with rules={rules}")
            if event_id == 1:
                with host_script.open("a") as commands:
                    commands.write("2 0 option driver_names 1\n")
                await_output(processes, lambda l: re.search(r"lobby seq=2 action=option cap=\d+ rules=0", l["host"]),
                             10, "host changed an active event")
            if enabled:
                logs = await_output(processes, lambda l: check_names(l, event_id), 80, "expected native human roof labels")
            else:
                # Give both game cameras a rendered world frame after GO before checking silence.
                logs = await_output(processes, lambda l: all(
                    re.search(r"world passes [1-9]", log.rsplit("GO (slot ", 1)[-1]) for log in l.values()
                ), 45, "no world render after GO")
                assert all(f"[netplay-names] event={event_id} " not in log for log in logs.values())
            if event_id == 1:
                with host_script.open("a") as commands:
                    commands.write("3 0 end\n")
                await_output(processes, lambda l: "event=1 state=lobby native_main=1" in l["host"], 60, "host did not return")
                with host_script.open("a") as commands:
                    commands.write("4 0 option driver_names 1\n")
        print(f"PASS {event}: host and guest identities, native camera samples and host-only rematch toggle")
    finally:
        for process, _ in processes.values():
            process.terminate()
        for process, _ in processes.values():
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
