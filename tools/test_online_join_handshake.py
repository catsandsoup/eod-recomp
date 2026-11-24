"""Run a real headless client through refusal, retry, and delayed lobby state."""

import argparse
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

from test_online_completion import await_output


MAGIC = 0x7D3E
JOIN, WELCOME, REFUSE, HEARTBEAT, LOBBY = 1, 2, 3, 4, 5


def header(kind):
    return struct.pack("!HBB", MAGIC, kind, 0)


def encode_string(value):
    if isinstance(value, str):
        value = value.encode()
    return bytes([len(value)]) + value


def read_string(packet, offset):
    if offset >= len(packet):
        raise ValueError("Missing string length")
    end = offset + 1 + packet[offset]
    if end > len(packet):
        raise ValueError("Incomplete string")
    return packet[offset + 1:end], end


def read_join(packet):
    # Use the actual client's protocol and content fingerprint. Each release can
    # increment the protocol without changing this fixture's connection setup.
    protocol = struct.unpack_from("!H", packet, 4)[0]
    _, name_end = read_string(packet, 6)
    content, _ = read_string(packet, name_end + 4)
    return protocol, content


def lobby(stamp, slots):
    result = header(LOBBY) + struct.pack("!IIB", stamp, 0, 0)
    result += encode_string("No-Rules Race") + bytes([0, len(slots)])
    for slot in slots:
        result += bytes([slot]) + encode_string("Loopback player") + bytes([0]) + encode_string("Moth") + bytes(2)
    return result + encode_string("Anoka") + bytes([0, 16, 7, 4, 4, 12])


def run_probe(client, host):
    start = time.monotonic()
    refused_at = None
    welcomed_at = None
    guest_addr = None
    protocol = None
    sent_valid = False
    while time.monotonic() - start < 50 and client.poll() is None:
        now = time.monotonic()
        if guest_addr is not None:
            stamp = int((now - start) * 1000)
            if now - welcomed_at >= 1.2:
                host.sendto(lobby(stamp, [0, 1]), guest_addr)
                sent_valid = True
            else:
                host.sendto(lobby(stamp, [0, 2]), guest_addr)
                host.sendto(lobby(stamp, [0, 1])[:-1], guest_addr)
        try:
            packet, addr = host.recvfrom(1500)
        except socket.timeout:
            continue
        if len(packet) < 4 or struct.unpack_from("!H", packet)[0] != MAGIC:
            continue
        if packet[2] == JOIN:
            if refused_at is None or time.monotonic() - refused_at < 1:
                if refused_at is None:
                    refused_at = time.monotonic()
                host.sendto(header(REFUSE) + bytes([1]), addr)
            elif guest_addr is None:
                guest_addr = addr
                protocol, content = read_join(packet)
                host.sendto(header(WELCOME) + bytes([1]) + b"\x12" * 16 + encode_string(content), addr)
                welcomed_at = time.monotonic()
        if packet[2] == HEARTBEAT:
            if not sent_valid:
                raise AssertionError("Client sent a lobby heartbeat before receiving valid details")
            if addr == guest_addr and packet[3] == 1:
                _, name_end = read_string(packet, 4)
                if packet[name_end]:
                    return protocol
    raise AssertionError("Client never marked Ready after retry and lobby hydration")


def check_ready_order(exe, disc, root, protocol):
    """A delayed Ready must not undo Unready or launch a player changing cars."""
    root.mkdir()
    commands = root / "commands"
    log = root / "game.log"
    env = dict(os.environ, EOD_NETPLAY_AUTO="host", EOD_NETPLAY_TEST_PEERS="2",
               EOD_NETPLAY_TEST_LOBBY=str(commands), EOD_NETPLAY_NO_INTERNET="1", EOD_NETPLAY_NO_LAN="1",
               EOD_NETPLAY_CAR="Algar", EOD_HEADLESS="1", EOD_MUTE="1", EOD_SKIP_MOVIES="1",
               EOD_MEMCARD=str(root / "card"), EOD_APP_SUPPORT=str(root / "support"),
               EOD_PAD_SCRIPT="400:0008,410:0000")
    with log.open("w") as output:
        game = subprocess.Popen([exe, "--headless", "--data", disc, "--config", root / "eod.ini",
                                 "--frames", "12000"], env=env, stdout=output, stderr=output)
    games = {"host": (game, log)}
    try:
        lines = await_output(games, lambda logs: "hosting on " in logs["host"], 40, "Host did not open")["host"]
        address = ("127.0.0.1", int(re.search(r"hosting on [^:\n]+:(\d+),", lines)[1]))
        content = re.search(r"content ([0-9a-f]{16})", lines)[1]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.connect(address)
            peer.settimeout(4)
            def receive(kind):
                until = time.monotonic() + 5
                while time.monotonic() < until:
                    packet = peer.recv(1536)
                    if packet[2] == kind:
                        return packet
                raise AssertionError(f"Missing host message {kind}")

            def join(version):
                peer.send(header(JOIN) + struct.pack("!H", version) + encode_string("Guest")
                          + struct.pack("!I", 0) + encode_string(content))

            join(protocol - 1)
            refusal = receive(REFUSE)
            assert refusal[4] == 1 and struct.unpack_from("!H", refusal, 5)[0] == protocol
            version, end = read_string(refusal, 7)
            assert re.fullmatch(rb"\d+(\.\d+)+", version) and end == len(refusal)
            join(protocol)
            player = receive(WELCOME)[4]
            def guest_ready(packet):
                _, end = read_string(packet, 13)  # after timestamp, event and phase
                count, index = packet[end + 1], end + 2
                for _ in range(count):
                    slot = packet[index]
                    _, index = read_string(packet, index + 1)
                    flags = packet[index]
                    _, index = read_string(packet, index + 1)
                    index += 2  # ping
                    if slot == player:
                        return bool(flags & 1)
                raise AssertionError("Admitted guest missing from host roster")

            def heartbeat(ready, serial):
                return (struct.pack("!HBB", MAGIC, HEARTBEAT, player) + encode_string("Guest")
                        + struct.pack("!BI", ready, 2) + encode_string("Algar")
                        + struct.pack("!BIBII", 0, 0, 1, 0, serial))

            receive(LOBBY)
            older = heartbeat(True, 1)
            for message, expected in ((older, True), (heartbeat(False, 2), False), (older, False)):
                peer.send(message)
                # Observe two consecutive publications after the command.
                receive(LOBBY)
                assert guest_ready(receive(LOBBY)) == expected, "Reordered Ready changed the host roster"
            commands.write_text("1 0 start\n")
            assert not guest_ready(receive(LOBBY)) and not guest_ready(receive(LOBBY))
            assert "action=start" not in log.read_text(), "Host launched an unready guest"
            peer.send(heartbeat(True, 3))
            await_output(games, lambda logs: "action=start" in logs["host"], 5, "Fresh Ready did not allow Start")
            print("The host ignored the stale Ready and launched only after a fresh Ready.")
    finally:
        game.terminate()
        try:
            game.wait(timeout=4)
        except subprocess.TimeoutExpired:
            game.kill()
            game.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path, help="Path to the current eod executable")
    parser.add_argument("disc", type=Path, help="Path to extracted, supported disc content")
    parser.add_argument("output", type=Path, help="New directory for the process log and test state")
    parser.add_argument("--host-order", action="store_true", help="also check Ready/Unready message ordering on a real host")
    args = parser.parse_args()
    exe, disc, output = (p.resolve() for p in (args.exe, args.disc, args.output))
    output.mkdir(parents=True, exist_ok=False)
    log_path = output / "game.log"
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host:
        host.bind(("127.0.0.1", 0))
        host.settimeout(0.07)
        port = host.getsockname()[1]
        env = dict(os.environ, EOD_NETPLAY_AUTO=f"join:127.0.0.1:{port}", EOD_NETPLAY_LIFECYCLE="1",
                   EOD_NETPLAY_NO_LAN="1", EOD_NETPLAY_NO_INTERNET="1", EOD_NETPLAY_CAR="Moth",
                   EOD_HEADLESS="1", EOD_MUTE="1", EOD_SKIP_MOVIES="1",
                   EOD_MEMCARD=str(output / "card"), EOD_APP_SUPPORT=str(output / "support"),
                   EOD_PAD_SCRIPT="360:0008,370:0000")
        with log_path.open("w") as log:
            client = subprocess.Popen([exe, "--headless", "--data", disc, "--config", output / "eod.ini",
                                       "--frames", "2000"], cwd=output, env=env, stdout=log, stderr=log)
        try:
            protocol = run_probe(client, host)
            live_log = log_path.read_text()
            states = re.findall(r"^\[lifecycle\].*? state=(\w+)", live_log, re.MULTILINE)
            retry = ["connecting", "offline", "connecting", "lobby"]
            if not any(states[i:i + len(retry)] == retry for i in range(len(states))):
                raise AssertionError(f"Unexpected join phases {states}")
            print(f"Protocol {protocol}: first join refused, retry waited for a valid lobby, then Ready succeeded.")
        except AssertionError as error:
            raise SystemExit(f"{error}. See {log_path}") from error
        finally:
            if client.poll() is None:
                client.terminate()
                try:
                    client.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    client.kill()
                    client.wait()
    if args.host_order:
        check_ready_order(exe, disc, output / "host-order", protocol)


if __name__ == "__main__":
    main()
