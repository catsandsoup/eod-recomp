"""Exercise a real incompatible guest and rejoin the same host using installed files."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

from test_online_completion import await_output, launch


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("exe", type=Path)
    cli.add_argument("disc", type=Path)
    cli.add_argument("root", type=Path)
    cli.add_argument("integrated", type=Path)
    content = cli.add_mutually_exclusive_group()
    content.add_argument("--host-integrated", action="store_true")
    content.add_argument("--missing-custom", action="store_true", help="host uses a package absent from the guest")
    cli.add_argument("--garage-code", action="store_true", help="withhold direct routes and rejoin via the same Garage Code")
    args = cli.parse_args()
    exe, disc, root, integrated = (p.resolve() for p in (args.exe, args.disc, args.root, args.integrated))
    root.mkdir(parents=True, exist_ok=False)
    games = {}
    service = None
    endpoint = None
    if not (integrated / "INTEGRATED.MANIFEST").is_file():
        raise ValueError("Supply an installed integrated content pack")
    try:
        def profile(use_pack):
            if use_pack:
                os.environ["EOD_MOD_DIR"] = str(integrated)
            else:
                os.environ.pop("EOD_MOD_DIR", None)
        if args.garage_code:
            service = subprocess.Popen([sys.executable, str(Path(__file__).with_name("garage_relay.py")),
                                        "--bind", "127.0.0.1", "--port", "0", "--relay-only"],
                                       stdout=subprocess.PIPE, text=True)
            endpoint = re.match(r"Garage relay listening on (127\.0\.0\.1:\d+)", service.stdout.readline())[1]
        profile(args.host_integrated)
        if args.missing_custom:
            custom = root / "custom"
            (custom / "DATA/CAR").mkdir(parents=True)
            (custom / "DATA/CAR/recovery-fixture.txt").write_text("content mismatch fixture")
            digest = "a" * 64
            (custom / "PROFILE.TXT").write_text(f"profile local\ngame {digest}\npackage derby-paints 2.1.0 manifest {digest}\n")
            os.environ["EOD_MOD_DIR"] = str(custom)
        launch(games, exe, disc, root, "host", "host", "", "No-Rules Race", 2, endpoint)
        address = r"hosting on [^:\n]+:(\d+), Garage Code (\w+)"
        log = await_output(games, lambda logs: re.search(address, logs["host"]), 50, "host failed to open")
        profile(not args.host_integrated and not args.missing_custom)
        script = root / "guest.commands"
        os.environ["EOD_NETPLAY_TEST_LOBBY"] = str(script)
        host = re.search(address, log["host"])
        invite = "joincode:" + host[2] if args.garage_code else "join:127.0.0.1:" + host[1]
        launch(games, exe, disc, root, "guest", invite, "", "No-Rules Race", 2, endpoint)
        if args.host_integrated:
            # The installed standalone-profile pointer is the one consumed by the production boot path.
            pointer = root / "guest" / "support" / "profiles" / "active_profile.txt"
            pointer.parent.mkdir(parents=True, exist_ok=True)
            pointer.write_text(str(integrated) + "\n")
        kind = "the integrated content pack" if args.host_integrated else "disc content without a gameplay pack"
        if args.missing_custom:
            logs = await_output(games, lambda logs: "installed profile match unavailable" in logs["guest"], 80,
                                "guest did not resolve the missing profile")
            assert "Host packages: derby-paints@2.1.0" in logs["guest"]
            assert "as player 1" not in logs["host"] and "restarting with a matching" not in logs["guest"]
            print("PASS: missing custom content showed the host package ID and version. The guest obtained no seat or restart action.")
            return
        logs = await_output(games, lambda logs: f"The host uses {kind}" in logs["guest"], 80, "missing host content details")
        assert "as player 1" not in logs["host"], "mismatched guest obtained a seat"
        script.write_text("1 0 match_content\n")
        logs = await_output(games, lambda logs: "restarting with a matching installed profile" in logs["guest"]
                            and re.search(r"role=2 event=0 state=lobby native_main=1", logs["guest"]),
                            70, "guest failed to relaunch and rejoin")
        assert "rejoining the saved lobby with the selected local content" in logs["guest"]
        host_ids = re.findall(r"\[netplay\] content ([a-f0-9]{16})", logs["host"])
        guest_ids = re.findall(r"\[netplay\] content ([a-f0-9]{16})", logs["guest"])
        assert len(guest_ids) >= 2 and guest_ids[0] != host_ids[-1] and guest_ids[-1] == host_ids[-1]
        assert logs["host"].count("as player 1") == 1
        if args.garage_code:
            assert "Garage Code route: Connected through the relay" in logs["guest"]
        if args.host_integrated:
            assert pointer.read_text() == str(integrated) + "\n"
        print(f"PASS: host required {kind}. Incompatible guest had no seat, then rejoined the same room "
              "with the matching local files in one player action.")
    finally:
        for process, _ in games.values():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
        if service:
            service.terminate()
            service.wait(timeout=5)


if __name__ == "__main__":
    main()
