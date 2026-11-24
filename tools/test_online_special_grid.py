"""Verify native Trailer Race hitches or Detention roles on two headless games."""

import argparse
from pathlib import Path
import re

from test_online_completion import await_output, launch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("mode", choices=("trailer", "detention"))
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    processes = {}
    event = "Trailer Race" if args.mode == "trailer" else "Detention"
    try:
        def start(name, route):
            launch(processes, args.exe.resolve(), args.disc.resolve(), root, name, route,
                   "360:objective", event, 1)

        start("host", "host")
        host_port = r"hosting on [^:\n]+:(\d+),"
        logs = await_output(processes, lambda logs: re.search(host_port, logs["host"]), 60, "host did not start")
        start("guest", "join:127.0.0.1:" + re.search(host_port, logs["host"])[1])
        logs = await_output(processes, lambda logs: all("event=2 state=lobby native_main=1" in log
                            for log in logs.values()), 220, "both events did not return to the party")
        observed = {}
        for name, log in logs.items():
            assert log.count("[netplay] GO (slot ") == 2, name
            if args.mode == "trailer":
                pairs = re.findall(r"\[netplay\] hitch slot=(\d+) tow=(\d+) trailer=(\d+)", log)
                grids = re.findall(r"native trailer grid drivers=(\d+) trailers=(\d+)", log)
                assert len(grids) == 2 and all(int(a) > 1 and a == b for a, b in grids), grids
                assert len(pairs) == sum(int(a) for a, _ in grids), pairs
                cursor = 0
                for size, _ in grids:
                    chunk = pairs[cursor:cursor + int(size)]
                    assert [int(s) for s, _, _ in chunk] == list(range(int(size))), chunk
                    assert all(tow != trailer for _, tow, trailer in chunk), chunk
                    cursor += int(size)
                observed[name] = pairs
            else:
                roles = re.findall(r"Detention owner=(\d+) slot=(\d+) vehicle=(\d+) bus_slot=(\d+) damage_factor=([\d.]+)", log)
                assert len(roles) == 4 and [r[0] for r in roles].count("0") == 2, roles
                for owner, slot, vehicle, bus_slot, damage in roles:
                    if owner == "0":
                        assert slot == bus_slot and 790 <= int(vehicle) <= 794 and float(damage) == 0, roles
                    else:
                        assert owner == "1" and slot != bus_slot and vehicle == "775" and float(damage) > 0, roles
                observed[name] = roles
        assert observed["host"] == observed["guest"], "the two worlds disagree on the physical assignments"
        print(f"PASS: both games agreed on {event} assignments through two starts and lobby returns.")
    finally:
        for process, _ in processes.values():
            if process.poll() is None:
                process.terminate()
        for process, _ in processes.values():
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
