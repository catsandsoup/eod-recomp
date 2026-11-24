"""Garage Code rendezvous and UDP relay. Run with Python 3.9+ on a reachable UDP port."""

import argparse
from dataclasses import dataclass, field
import hashlib
import hmac
import os
import select
import socket
import struct
import time


MAGIC = b"EOR1"
LEASE = 30
ADMISSION_SECONDS = 10
MAX_ROOMS = 1024
MAX_MEMBERS = 64  # includes temporary join attempts awaiting game admission
MAX_PAYLOAD = 1500
REGISTER = (b"H", b"J", b"j")


@dataclass
class Member:
    addr: tuple
    token: bytes
    key: bytes
    route: int
    last_seen: float
    attempt: bytes = b""
    admission_until: float = 0
    notice_at: float = 0


@dataclass
class Room:
    host: Member
    members: dict = field(default_factory=dict)
    next_route: int = 2
    aliases: set = field(default_factory=set)
    players: int = 1
    generation: bytes = field(default_factory=lambda: os.urandom(4))


class Relay:
    def __init__(self, udp, direct=True):
        self.udp = udp
        self.direct = direct
        self.secret = os.urandom(32)
        self.rooms = {}
        self.aliases = {}
        self.addresses = {}

    def send(self, addr, kind, token, payload=b""):
        self.udp.sendto(MAGIC + kind + token + payload, addr)

    def cookie(self, addr, request, bucket):
        address = socket.inet_aton(addr[0]) + struct.pack("!HQ", addr[1], bucket)
        return hmac.new(self.secret, address + request, hashlib.sha256).digest()[:16]

    def notify_admission(self, room, guest, now):
        remaining = max(0, min(ADMISSION_SECONDS * 1000, int((guest.admission_until - now) * 1000)))
        self.send(room.host.addr, b"N", room.host.token,
                  room.generation + guest.attempt + struct.pack("!H", remaining))
        guest.notice_at = now

    def finish_admission(self, member):
        if member.admission_until:
            member.admission_until = 0
            self.notify_admission(self.rooms[member.key], member, 0)

    def remove(self, member):
        room = self.rooms[member.key]
        if member is room.host:
            for guest in room.members.values():
                self.addresses.pop(guest.addr, None)
            for key in room.aliases:
                self.aliases.pop(key, None)
            del self.rooms[member.key]
        else:
            self.finish_admission(member)
            room.members.pop(member.route, None)
        self.addresses.pop(member.addr, None)

    def expire(self, now):
        for member in list(self.addresses.values()):
            if member.addr not in self.addresses:
                continue
            if now - member.last_seen >= LEASE:
                self.remove(member)
            elif member.admission_until:
                if now >= member.admission_until:
                    self.finish_admission(member)
                elif now - member.notice_at >= 1:
                    self.notify_admission(self.rooms[member.key], member, now)

    def register(self, addr, kind, token, body, now):
        if len(body) != 36:
            return
        key, proof = body[:20], body[20:]
        request = kind + token + key
        bucket = int(now // LEASE)
        if proof == bytes(16):
            self.send(addr, b"C", token, self.cookie(addr, request, bucket))
            return
        if not any(hmac.compare_digest(proof, self.cookie(addr, request, epoch))
                   for epoch in (bucket, max(0, bucket - 1))):
            self.send(addr, b"C", token, self.cookie(addr, request, bucket))
            return
        if kind != b"H":
            key = self.aliases.get(key, key)
        room = self.rooms.get(key)
        member = self.addresses.get(addr)
        if member and member.key == key and member.token == token:
            if (kind == b"H") != (member is room.host):
                return
        else:
            if kind == b"H" and room is not None:
                self.send(addr, b"E", token, b"C")
                return
            if kind != b"H" and room is None:
                self.send(addr, b"E", token, b"N")
                return
            if ((kind == b"H" and len(self.rooms) >= MAX_ROOMS) or
                    (kind != b"H" and len(room.members) >= MAX_MEMBERS)):
                self.send(addr, b"E", token, b"F")
                return
            if member:
                return
            if kind == b"H":
                member = Member(addr, token, key, 1, now)
                self.rooms[key] = Room(member)
            else:
                route = room.next_route
                # Never reuse a route during a room's lifetime. Delayed packets
                # from an old connection cannot reach a different occupant.
                if route > 65535:
                    self.send(addr, b"E", token, b"F")
                    return
                room.next_route += 1
                member = Member(addr, token, key, route, now)
                # This identifier correlates the game JOIN with this reservation.
                # It cannot be used as a relay credential and carries no address.
                member.attempt = hashlib.sha1(b"TDEOD-EVE-NET-1/A/" + token.hex().encode()).digest()[:16]
                member.admission_until = now + ADMISSION_SECONDS
                room.members[route] = member
            self.addresses[addr] = member
        member.last_seen = now
        self.send(addr, b"R", token, self.rooms[member.key].generation)
        if kind != b"H" and member.admission_until:
            self.notify_admission(room, member, now)
        if kind == b"J" and self.direct:
            for target, peer in ((member, room.host), (room.host, member)):
                candidate = socket.inet_aton(peer.addr[0]) + struct.pack("!H", peer.addr[1])
                self.send(target.addr, b"P", target.token, candidate)

    def handle(self, packet, addr, now):
        if not packet.startswith(MAGIC) or not 21 <= len(packet) <= 27 + MAX_PAYLOAD:
            return
        kind, token, body = packet[4:5], packet[5:21], packet[21:]
        if kind in REGISTER:
            self.register(addr, kind, token, body, now)
            return
        if kind == b"Q" and len(body) == 20:
            room = self.rooms.get(self.aliases.get(body))
            if room is not None:
                self.send(addr, b"Q", token, body + bytes([room.players]))
            return
        member = self.addresses.get(addr)
        if member is None or not hmac.compare_digest(member.token, token):
            return
        if kind == b"A" and 1 <= len(body) <= 241 and (len(body) - 1) % 20 == 0:
            room = self.rooms[member.key]
            if member is room.host:
                room.players = min(16, max(1, body[0]))
                keys = {body[i:i + 20] for i in range(1, len(body), 20)}
                for key in room.aliases - keys:
                    self.aliases.pop(key, None)
                room.aliases = {key for key in keys if self.aliases.get(key, member.key) == member.key}
                for key in room.aliases:
                    self.aliases[key] = member.key
            return
        if kind == b"X" and not body:
            self.remove(member)
            return
        if kind == b"F" and not body:
            self.finish_admission(member)
            return
        if kind == b"K" and not body:
            member.last_seen = now
            self.send(addr, b"R", token, self.rooms[member.key].generation)
            return
        if kind != b"D" or len(body) < 10 or body[6:8] != b"\x7d\x3e":
            return
        room = self.rooms[member.key]
        if body[:4] != room.generation:
            return
        route, = struct.unpack_from("!H", body, 4)
        target = room.members.get(route) if member is room.host else room.host if route == 1 else None
        if target is None:
            return
        member.last_seen = now
        self.send(target.addr, b"D", target.token, room.generation + struct.pack("!H", member.route) + body[6:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=27016)
    parser.add_argument("--relay-only", action="store_true",
                        help="keep peer addresses private and use the relay for every Garage Code")
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind((args.bind, args.port))
        # Keep the queue draining when a host sends a full grid of snapshots.
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
        service = Relay(udp, direct=not args.relay_only)
        print(f"Garage relay listening on {udp.getsockname()[0]}:{udp.getsockname()[1]}", flush=True)
        sweep = time.monotonic()
        while True:
            if select.select([udp], [], [], 1)[0]:
                try:
                    packet, addr = udp.recvfrom(65535)
                    service.handle(packet, addr, time.monotonic())
                except (ConnectionResetError, ConnectionRefusedError):
                    pass
            now = time.monotonic()
            if now >= sweep:
                service.expire(now)
                sweep = now + 1


if __name__ == "__main__":
    main()
