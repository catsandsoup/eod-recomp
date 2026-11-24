"""Exercise room admission, forwarding, isolation, presence and expiry."""

from pathlib import Path
import selectors
import socket
import struct
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from garage_relay import ADMISSION_SECONDS, LEASE, MAGIC, Relay


class Capture:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))


class RelayTest(unittest.TestCase):
    def setUp(self):
        self.udp = Capture()
        self.relay = Relay(self.udp)
        self.room = b"r" * 20
        self.now = 70

    def packet(self, role, kind, body=b"", token=None):
        addr = ("127.0.0.1", 5000 + role)
        token = bytes([role]) * 16 if token is None else token
        self.udp.sent.clear()
        self.relay.handle(MAGIC + kind + token + body, addr, self.now)
        return list(self.udp.sent)

    def admit(self, role, op, key):
        replies = self.packet(role, op, key + bytes(16))
        self.assertEqual(replies[0][1][4:5], b"C")
        return self.packet(role, op, key + replies[0][1][21:])

    def test_room_lifecycle(self):
        self.assertEqual(self.admit(1, b"H", self.room)[0][1][4:5], b"R")
        self.assertEqual(self.admit(4, b"H", self.room)[0][1][21:], b"C")
        self.assertEqual(self.admit(4, b"j", b"?" * 20)[0][1][21:], b"N")
        self.assertEqual(self.admit(2, b"j", self.room)[0][1][4:5], b"R")
        admitted = self.admit(3, b"J", self.room)
        self.assertEqual([m[1][4:5] for m in admitted], [b"R", b"N", b"P", b"P"])
        generation = self.relay.rooms[self.room].generation
        payload = bytes.fromhex("7d3e0100") + b"fixture"
        self.assertEqual(self.packet(2, b"D", generation + b"\0\1" + payload),
                         [(("127.0.0.1", 5001), MAGIC + b"D" + bytes([1])*16 + generation + b"\0\2" + payload)])
        self.assertEqual(self.packet(1, b"D", generation + b"\0\3" + payload)[0][0][1], 5003)
        self.assertEqual(self.packet(2, b"D", generation + b"\0\3" + payload), [])
        self.assertEqual(self.packet(2, b"D", generation + b"\0\1" + payload, token=bytes(16)), [])
        self.assertEqual(self.packet(2, b"D", bytes(v ^ 255 for v in generation) + bytes([0, 1]) + payload), [])
        self.packet(3, b"X")
        self.assertEqual(self.packet(1, b"D", generation + b"\0\3" + payload), [])
        self.admit(3, b"j", self.room)
        self.assertEqual(self.packet(1, b"D", generation + b"\0\3" + payload), [])
        self.assertEqual(self.packet(1, b"D", generation + b"\0\4" + payload)[0][0][1], 5003)
        self.packet(1, b"X")
        self.assertFalse(self.relay.addresses)
        self.assertFalse(self.relay.rooms)

    def test_remembered_friend_and_expiry(self):
        key = b"f" * 20
        self.admit(1, b"H", self.room)
        self.packet(1, b"A", bytes([2]) + key)
        self.assertEqual(self.packet(9, b"Q", key)[0][1][21:], key + bytes([2]))
        self.assertEqual(self.admit(2, b"j", key)[0][1][4:5], b"R")
        self.now += LEASE + 1
        self.relay.expire(self.now)
        self.assertFalse(self.relay.addresses)
        self.assertEqual(self.packet(9, b"Q", key), [])

    def test_registration_uses_requesting_socket(self):
        cookie = self.packet(1, b"H", self.room + bytes(16))[0][1][21:]
        token = bytes([1]) * 16
        self.packet(2, b"H", self.room + cookie, token)
        self.assertFalse(self.relay.rooms)

    def test_relay_only_admission_cancel_accept_and_timeout(self):
        self.relay.direct = False
        self.admit(1, b"H", self.room)
        pending = {}
        for guest in (2, 3, 4):
            replies = self.admit(guest, b"J", self.room)
            self.assertEqual([p[1][4:5] for p in replies], [b"R", b"N"])
            dest, notice = replies[1]
            self.assertEqual(dest, ("127.0.0.1", 5001))
            self.assertEqual(len(notice), 21 + 4 + 16 + 2)
            self.assertEqual(int.from_bytes(notice[-2:], "big"), ADMISSION_SECONDS * 1000)
            pending[guest] = notice[:-2]
        self.assertEqual(len(set(pending.values())), 3)
        self.assertEqual(self.packet(2, b"X")[0][1], pending[2] + bytes(2))
        self.assertEqual(self.packet(3, b"F")[0][1], pending[3] + bytes(2))
        self.udp.sent.clear()
        self.relay.expire(self.now + 2)
        self.assertEqual(self.udp.sent[0][1], pending[4] + struct.pack("!H", (ADMISSION_SECONDS - 2) * 1000))
        self.udp.sent.clear()
        self.relay.expire(self.now + ADMISSION_SECONDS)
        self.assertEqual(self.udp.sent[0][1], pending[4] + bytes(2))
        self.assertFalse(self.packet(4, b"F"))


def serve_lost_welcome():
    """Advertise a test gateway per guest that passes probes and drops WELCOME.

    The host receives the first JOIN and admits a pending seat. Both sides
    then have to resume that same seat through the actual relay.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, selectors.DefaultSelector() as selector:
        udp.bind(("127.0.0.1", 0))
        selector.register(udp, selectors.EVENT_READ)
        gateways = {}

        class LostWelcomeRelay(Relay):
            def send(self, addr, kind, token, payload=b""):
                if kind == b"P" and self.addresses[addr].route != 1:
                    if addr not in gateways:
                        gateway = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        gateway.bind(("127.0.0.1", 0))
                        host = (socket.inet_ntoa(payload[:4]), struct.unpack("!H", payload[4:])[0])
                        gateways[addr] = gateway
                        selector.register(gateway, selectors.EVENT_READ, (host, addr))
                    local_ip, local_port = gateways[addr].getsockname()
                    payload = socket.inet_aton(local_ip) + struct.pack("!H", local_port)
                super().send(addr, kind, token, payload)

        service = LostWelcomeRelay(udp)
        print(f"Garage relay listening on 127.0.0.1:{udp.getsockname()[1]}", flush=True)
        while True:
            for key, _ in selector.select(1):
                packet, source = key.fileobj.recvfrom(65535)
                if key.fileobj is udp:
                    service.handle(packet, source, time.monotonic())
                else:
                    host, guest = key.data
                    if source == guest:
                        key.fileobj.sendto(packet, host)
                    elif source == host and len(packet) >= 4 and packet[2] != 2:
                        key.fileobj.sendto(packet, guest)


if __name__ == "__main__":
    if sys.argv[1:] == ["--drop-welcome"]:
        serve_lost_welcome()
    else:
        unittest.main()
