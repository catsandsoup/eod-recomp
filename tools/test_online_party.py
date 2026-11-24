"""Exercise online party recovery before, during and after events with three games."""

import argparse
import os
from pathlib import Path
import re
import subprocess

from test_online_completion import await_output, launch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("root", type=Path)
    faults = parser.add_mutually_exclusive_group()
    faults.add_argument("--abrupt-host", action="store_true",
                        help="terminate the test host during the rematch and verify both guests recover")
    faults.add_argument("--lose-loading-guest", action="store_true",
                        help="lose a guest during loading and then start a new round with a replacement")
    faults.add_argument("--lose-loading-host", action="store_true",
                        help="lose the host before GO, then form another party using the two running guests")
    faults.add_argument("--lose-results-guest", action="store_true",
                        help="lose a guest on the results screen, then finish another event with the survivors")
    args = parser.parse_args()
    exe, disc, root = (path.resolve() for path in (args.exe, args.disc, args.root))
    root.mkdir(parents=True, exist_ok=False)
    games = {}

    def start(name, mode, finish):
        os.environ["EOD_NETPLAY_TEST_LOBBY"] = str(root / (name + ".commands"))
        os.environ["EOD_NETPLAY_TEST_EPHEMERAL_PORT"] = "1"
        require_two = args.lose_loading_guest or args.lose_loading_host or args.lose_results_guest
        launch(games, exe, disc, root, name, mode, finish, "No-Rules Race", 2 if require_two else 1)

    def wait(check, reason, timeout=120):
        return await_output(games, check, timeout, reason)

    def crash(name):
        process, log_path = games.pop(name)
        process.kill()
        process.wait(timeout=5)
        return log_path.read_text(errors="replace")

    def command(name, sequence, action):
        (root / (name + ".commands")).write_text(f"{sequence} 0 {action}\n")

    try:
        boundary = args.lose_loading_host or args.lose_results_guest
        start("host", "host", "180:finish" if boundary else "1500:finish")
        pattern = r"hosting on [^:\n]+:(\d+),"
        logs = wait(lambda logs: re.search(pattern, logs["host"]), "host did not open")
        address = "join:127.0.0.1:" + re.search(pattern, logs["host"])[1]
        start("driver", address, "270:finish" if boundary else "")
        if args.lose_loading_host:
            start("friend", address, "360:finish")
            wait(lambda logs: all("event=1 state=loading native_main=0" in log for log in logs.values()),
                 "all three games must have left their native menu before the host is killed")
            host_log = crash("host")
            assert "[netplay] GO (slot " not in host_log, "host reached GO before the loading fault"
            logs = wait(lambda logs: all("role=0 event=1 state=offline native_main=1" in log for log in logs.values()),
                        "loading host loss did not restore both guests to the native main menu", 30)
            for log in logs.values():
                assert "[netplay] GO (slot " not in log and "state=results" not in log
                assert "offline retry=1 reason=Lost connection to the host." in log, "host loss reason missing"
            command("driver", 1, "host")
            logs = wait(lambda logs: re.search(pattern, logs["driver"]), "recovered guest could not host")
            port = re.search(pattern, logs["driver"])[1]
            command("friend", 1, f"join 127.0.0.1:{port}")
            wait(lambda logs: "joined from" in logs["driver"] and
                 logs["friend"].count("role=2 event=0 state=lobby native_main=1") == 2,
                 "both surviving games did not join the reformed party")
            command("driver", 2, "start")
            logs = wait(lambda logs: all("event=1 state=results" in log and
                                         "event=1 state=lobby native_main=1" in log.split("state=results")[-1]
                                         for log in logs.values()), "reformed party could not complete a new race")
            rows = [re.findall(r"\[netplay\] result slot=(\d+) status=(\d+) rank=(\d+)", log)
                    for log in logs.values()]
            assert rows[0] == rows[1] and [(s, r) for _, s, r in rows[0]] == [("1", "1"), ("1", "2")]
            assert all(log.count("[netplay] GO (slot ") == 1 for log in logs.values())
            print("PASS: Both guests lost the host before GO, received the retryable reason, "
                  "formed a new party in the same processes, finished and returned to their lobby.")
            return
        if args.lose_results_guest:
            start("departing", address, "360:finish")
            logs = wait(lambda logs: all("event=1 state=results" in log for log in logs.values()),
                        "all three must receive the standings before a guest is killed")
            standings = r"\[netplay\] result slot=(\d+) status=(\d+) rank=(\d+)"
            first = re.findall(standings, logs["host"])
            assert [(s, r) for _, s, r in first] == [("1", "1"), ("1", "2"), ("1", "3")]
            assert all(re.findall(standings, log) == first for log in logs.values())
            departed = crash("departing")
            assert "event=1 state=lobby native_main=1" not in departed
            wait(lambda logs: "timed out" in logs["host"] and
                 all("event=1 state=lobby native_main=1" in log for log in logs.values()),
                 "remaining players did not return to the lobby after the guest timed out", 30)
            command("host", 1, "start")
            logs = wait(lambda logs: all("event=2 state=lobby native_main=1" in log for log in logs.values()),
                        "the two surviving players could not finish and return from their rematch")
            for log in logs.values():
                assert log.count("[netplay] GO (slot ") == 2
                assert log.count("event=1 state=results") == log.count("event=2 state=results") == 1
            second = re.findall(standings, logs["host"])[len(first):]
            assert len(second) == 2 and [(s, r) for _, s, r in second] == [("1", "1"), ("1", "2")]
            assert re.findall(standings, logs["driver"]) == first + second
            assert re.search(r"role=1 event=2 state=racing native_main=0 peers=1", logs["host"])
            print("PASS: Three identical standings survived a guest loss during results. The two "
                  "remaining players finished the next event and returned with matching two-driver standings.")
            return
        if args.lose_loading_guest:
            start("departing", address, "")
            wait(lambda logs: all("event=1 state=loading" in log for log in logs.values()),
                 "first grid never reached loading")
            crash("departing")
            logs = wait(lambda logs: all("event=1 state=lobby native_main=1" in log for log in logs.values()),
                        "loss during loading did not restore the surviving party", 50)
            assert all("[netplay] GO (slot " not in log for log in logs.values()), "incomplete grid launched"
            start("replacement", address, "")
            wait(lambda logs: all("event=2 state=racing" in log for log in logs.values()),
                 "the surviving party could not launch with a replacement")
            print("PASS: loss during loading cancelled the grid, restored the existing party and allowed a new three-car start.")
            return
        wait(lambda logs: all("[netplay] GO (slot " in log for log in logs.values()), "first grid did not start")
        start("late", address, "")
        wait(lambda logs: "joined lobby for next event during event 1" in logs["late"], "late driver never reached the waiting lobby")
        (root / "driver.commands").write_text("1 0 withdraw\n")
        wait(lambda logs: "withdrew from event 1" in logs["host"] and
             "event=1 state=lobby native_main=1" in logs["driver"], "withdrawal did not preserve the party")
        logs = wait(lambda logs: all("event=2 state=racing" in log for log in logs.values()), "three drivers never started the rematch", 180)
        assert "status=6 rank=0" in logs["host"], "withdrawn driver missing from first results"
        assert "[netplay] t=600 own slot" in logs["host"], "host stopped before finishing"
        assert logs["late"].count("[netplay] GO (slot ") == 1, "late driver entered the running grid"
        for name in ("driver", "host"):
            assert logs[name].count("[netplay] GO (slot ") == 2, name
        if args.abrupt_host:
            crash("host")
        else:
            (root / "host.commands").write_text("1 0 disconnect\n")
        wait(lambda logs: all(re.search(r"role=0 event=2 state=offline native_main=1", log)
                              for log in logs.values()), "host loss did not restore the remaining native menus", 30)
        print("PASS: Late guest waited, withdrawn driver stayed, all three started the rematch, "
              "and the remaining clients reached their offline menus after host loss.")
    finally:
        for proc, _ in games.values():
            if proc.poll() is None:
                proc.terminate()
        for proc, _ in games.values():
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    main()
