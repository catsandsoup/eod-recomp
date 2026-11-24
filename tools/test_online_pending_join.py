"""Check pre-admission Start with a real host and a controlled Garage Code guest."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
from unittest.mock import patch

from test_online_completion import await_output, launch
from test_online_join_handshake import encode_string, header, HEARTBEAT, JOIN, WELCOME


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("release", choices=("cancel", "timeout", "admit"))
    parser.add_argument("--direct", action="store_true", help="also advertise direct candidates")
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True)
    commands = root / "commands"
    command = [sys.executable, str(Path(__file__).with_name("garage_relay.py")), "--bind", "127.0.0.1", "--port", "0"]
    if not args.direct:
        command.append("--relay-only")
    relay = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
    games = {}
    try:
        endpoint = re.search(r"127\.0\.0\.1:(\d+)", relay.stdout.readline())[0]
        with patch.dict(os.environ, EOD_NETPLAY_TEST_LOBBY=str(commands)):
            launch(games, args.exe.resolve(), args.disc.resolve(), root, "host", "host", "", "No-Rules Race", 2, endpoint)

        def wait(needle, timeout=15):
            return await_output(games, lambda logs: needle in logs["host"], timeout, needle)["host"]

        log = wait("state=lobby native_main=1", 45)
        code = re.search(r"Garage Code ([A-Z0-9]{5})", log)[1]
        content = re.search(r"content ([0-9a-f]{16})", log)[1]
        key = hashlib.sha1(b"TDEOD-EVE-NET-1/R/" + code.encode()).digest()
        token = os.urandom(16)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as guest:
            server = ("127.0.0.1", int(endpoint.split(":")[1]))
            guest.settimeout(4)
            def send(kind, body=b""):
                guest.sendto(b"EOR1" + kind + token + body, server)

            send(b"J", key + bytes(16))
            challenge = guest.recv(1536)
            assert challenge[:21] == b"EOR1C" + token
            send(b"J", key + challenge[21:])
            allocation = guest.recv(1536)
            assert allocation[:21] == b"EOR1R" + token
            address = allocation[21:] + struct.pack("!H", 1)
            direct = None
            if args.direct:
                candidate = guest.recv(1536)
                assert candidate[:21] == b"EOR1P" + token
                direct = (socket.inet_ntoa(candidate[21:25]), int.from_bytes(candidate[25:27], "big"))
            def game_packet(body):
                if direct:
                    guest.sendto(body, direct)
                else:
                    send(b"D", address + body)
            wait("Garage Code join pending")
            commands.write_text("1 0 start\n")
            time.sleep(0.7)
            assert "action=start" not in games["host"][1].read_text(), "Host started alone during admission"
            before = time.monotonic()
            if args.release == "cancel":
                send(b"X")
            elif args.release == "admit":
                wire = (Path(__file__).resolve().parents[1] / "src/netplay_wire.h").read_text()
                protocol = int(re.search(r"kProtocol = (\d+)", wire)[1])
                attempt = hashlib.sha1(b"TDEOD-EVE-NET-1/A/" + token.hex().encode()).digest()[:16].hex()
                game_packet(header(JOIN) + struct.pack("!H", protocol) + encode_string("Guest")
                            + bytes(4) + encode_string(content) + encode_string(attempt))
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    packet = guest.recv(1536)
                    if packet.startswith(b"EOR1D"):
                        packet = packet[27:]
                    if packet.startswith(header(WELCOME)):
                        slot = packet[4]
                        break
                else:
                    raise AssertionError("Host never welcomed the reserved guest")
                # No service F. Repeated notifications for the same successful
                # JOIN must not re-enable the hold after the player is Ready.
                wait("Garage Code join cleared", 3)
                time.sleep(1.2)
                assert "action=start" not in games["host"][1].read_text(), "An admitted guest was still unready"
                hb = (struct.pack("!HBB", 0x7d3e, HEARTBEAT, slot) + encode_string("Guest")
                      + struct.pack("!BI", 1, 2) + encode_string("Algar") + struct.pack("!BIBII", 0, 0, 1, 0, 1))
                game_packet(hb)
            log = wait("action=start", 13 if args.release == "timeout" else 3)
            elapsed = time.monotonic() - before
            assert "Garage Code join cleared" in log
            if args.release == "timeout":
                assert elapsed >= 8, f"abandoned reservation expired early after {elapsed:.2f}s"
            if args.release != "admit":
                wait("[netplay] GO (slot ", 60)
            print(f"PASS: Start waited for the pending Garage Code guest; {args.release} released the gate in {elapsed:.2f}s.")
    finally:
        for game, _ in games.values():
            game.terminate()
            game.wait(timeout=8)
        relay.terminate()
        relay.wait(timeout=5)


if __name__ == "__main__":
    main()
