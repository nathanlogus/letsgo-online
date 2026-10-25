#!/usr/bin/env python3
"""Small, ephemeral TCP relay for Let's Go Recomp's social co-op prototype.

The relay forwards presence snapshots, emotes and voluntary social invitations. It never receives save data,
Pokémon, inventory, or story state. See docs/COOP-ONLINE.md for the wire format.

Environment:
  COOP_RELAY_HOST       bind address (default 127.0.0.1)
  COOP_RELAY_PORT       TCP game port (default 7381)
  COOP_RELAY_HEALTH     localhost HTTP health port (default 7382; 0 disables)
  COOP_RELAY_MAX_ROOMS  maximum rooms in memory (default 100)
"""
from __future__ import annotations

import asyncio
import json
import os
import secrets
import signal
import socket
import struct
import time
from dataclasses import dataclass, field

PROTO = 5
RECONNECT_GRACE = 30
TEAM_INVITE_SECONDS = 45  # Leave time to navigate Communicate and return to the overworld.
MAX_FRAME = 256
MAX_ROOMS = max(1, int(os.getenv("COOP_RELAY_MAX_ROOMS", "100")))
CODE_CHARS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def frame(kind: bytes, body: bytes = b"") -> bytes:
    payload = kind + body
    if not payload or len(payload) > MAX_FRAME:
        raise ValueError("bad frame size")
    return struct.pack("<H", len(payload)) + payload


async def read_frame(reader: asyncio.StreamReader) -> bytes:
    n = struct.unpack("<H", await reader.readexactly(2))[0]
    if n < 1 or n > MAX_FRAME:
        raise ConnectionError("bad frame size")
    return await reader.readexactly(n)


def clean_name(data: bytes) -> str:
    name = data.decode("utf-8", "replace")
    name = "".join(c for c in name if c.isprintable() and c not in "\r\n\t")
    name = name.strip().encode("utf-8")[:24].decode("utf-8", "ignore")
    return name or "Trainer"


@dataclass(eq=False)
class Peer:
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    name: str
    slot: int
    room: "Room"
    identity: bytes = b""
    token: bytes = field(default_factory=lambda: secrets.token_bytes(16))
    connected: bool = True
    joined_at: float = field(default_factory=time.monotonic)
    pose: tuple[int, int, int, int] | None = None
    pose_at: float = 0
    activity: int = 0

    def send(self, kind: bytes, body: bytes = b"") -> None:
        if self.connected and not self.writer.is_closing():
            if self.writer.transport.get_write_buffer_size() > 262144:
                self.writer.close()
                return
            self.writer.write(frame(kind, body))


@dataclass
class Room:
    code: str
    fingerprint: tuple[int, int]
    peers: list[Peer] = field(default_factory=list)
    owner: bytes = b""
    locked: bool = False
    removed: set[bytes] = field(default_factory=set)
    team_state: int = 0
    team_id: int = 0
    team_from: int = 255
    team_until: float = 0


class Relay:
    def team_status(self, room: Room, reason: int = 0, target: Peer | None = None) -> None:
        remaining=max(0,int((room.team_until-time.monotonic())*1000)) if room.team_state==1 else 0
        body=struct.pack('<BBBII',room.team_state,room.team_from,reason,room.team_id,remaining)
        for peer in ([target] if target else room.peers):
            peer.send(b'U',body)

    def clear_team(self, room: Room, reason: int) -> None:
        room.team_state=0;room.team_from=255;room.team_until=0
        self.team_status(room,reason)

    async def expire_team(self, room: Room, invitation: int) -> None:
        await asyncio.sleep(TEAM_INVITE_SECONDS)
        if room.team_state==1 and room.team_id==invitation:
            self.clear_team(room,5)

    @staticmethod
    def team_near(a: Peer, b: Peer) -> bool:
        now=time.monotonic()
        return (a.activity==b.activity==1 and a.pose is not None and b.pose is not None and
                now-a.pose_at<1 and now-b.pose_at<1 and a.pose[0]==b.pose[0] and
                sum((a.pose[i]-b.pose[i])**2 for i in (1,2,3))<=3000**2)

    def team_action(self, peer: Peer, command: int, invitation: int) -> None:
        room=peer.room
        other=next((p for p in room.peers if p is not peer and p.connected),None)
        if command==0:
            if room.team_state or other is None:
                self.team_status(room,3,peer);return
            if not self.team_near(peer,other):
                self.team_status(room,4,peer);return
            room.team_id=secrets.randbits(32) or 1
            room.team_state=1;room.team_from=peer.slot;room.team_until=time.monotonic()+TEAM_INVITE_SECONDS
            self.team_status(room)
            asyncio.create_task(self.expire_team(room,room.team_id))
        elif invitation!=room.team_id or not room.team_state:
            self.team_status(room,7,peer)
        elif command==1:
            if room.team_state!=1 or peer.slot==room.team_from or other is None:
                self.team_status(room,7,peer);return
            if time.monotonic()>=room.team_until:
                self.clear_team(room,5);return
            if not self.team_near(peer,other):
                self.team_status(room,4,peer);return
            room.team_state=2;room.team_until=0;self.team_status(room)
        elif command==2 and room.team_state==1 and peer.slot!=room.team_from:
            self.clear_team(room,1)
        elif command==3 and (room.team_state==2 or peer.slot==room.team_from):
            self.clear_team(room,2)
        else:
            self.team_status(room,7,peer)

    def __init__(self) -> None:
        self.rooms: dict[str, Room] = {}
        self.connections = 0
        self.started = time.monotonic()
        self.writers: set[asyncio.StreamWriter] = set()

    def new_code(self) -> str:
        while True:
            code = "LG-" + "".join(secrets.choice(CODE_CHARS) for _ in range(6))
            if code not in self.rooms:
                return code

    def health(self) -> dict[str, object]:
        return {
            "ok": True,
            "protocol": PROTO,
            "rooms": len(self.rooms),
            "players": sum(p.connected for room in self.rooms.values() for p in room.peers),
            "connections": self.connections,
            "uptime": int(time.monotonic() - self.started),
        }

    async def reject(self, writer: asyncio.StreamWriter, why: str) -> None:
        data = why.encode("utf-8")[:96]
        writer.write(frame(b"X", bytes([len(data)]) + data))
        try:
            await writer.drain()
        except (ConnectionError, OSError):
            pass

    def close_room(self, room: Room, why: str) -> None:
        self.rooms.pop(room.code, None)
        for other in list(room.peers):
            data = why.encode()[:96]
            other.send(b"X", bytes([len(data)]) + data)
            other.writer.close()
        room.peers.clear()

    async def expire(self, peer: Peer) -> None:
        await asyncio.sleep(RECONNECT_GRACE)
        room = peer.room
        # A reconnect replaces the old peer object, so its expiry is harmless.
        if peer.connected or peer not in room.peers:
            return
        if peer.identity == room.owner:
            self.close_room(room, "Host disconnected; continue playing solo")
        else:
            room.peers.remove(peer)
            if not room.peers:
                self.rooms.pop(room.code, None)

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer: Peer | None = None
        leaving = False
        self.connections += 1
        self.writers.add(writer)
        try:
            sock = writer.get_extra_info("socket")
            if sock is not None:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            # Connects that never identify a room are discarded quickly.
            hello = await asyncio.wait_for(read_frame(reader), timeout=10)
            if len(hello) < 20 or hello[0:1] not in (b"C", b"J", b"R"):
                await self.reject(writer, "expected create or join")
                return
            kind = hello[0:1]
            version = hello[1]
            title_id, build_id = struct.unpack_from("<QQ", hello, 2)
            if version != PROTO:
                await self.reject(writer, "protocol mismatch")
                return
            fingerprint = (title_id, build_id)
            off = 18
            room_code = ""
            if kind in (b"J", b"R"):
                if off >= len(hello):
                    await self.reject(writer, "missing room code")
                    return
                ncode = hello[off]
                off += 1
                if not 1 <= ncode <= 16 or off + ncode > len(hello):
                    await self.reject(writer, "bad room code")
                    return
                room_code = hello[off:off + ncode].decode("ascii", "ignore").upper()
                off += ncode
            if off >= len(hello):
                await self.reject(writer, "missing player name")
                return
            nname = hello[off]
            off += 1
            if not 1 <= nname <= 48 or off + nname + 32 != len(hello):
                await self.reject(writer, "bad player name")
                return
            name = clean_name(hello[off:off + nname])
            identity = hello[off + nname:off + nname + 16]
            token = hello[off + nname + 16:]
            if identity == bytes(16):
                await self.reject(writer, "missing player identity")
                return
            previous = None

            if kind == b"C":
                if len(self.rooms) >= MAX_ROOMS:
                    await self.reject(writer, "relay is full")
                    return
                room = Room(self.new_code(), fingerprint, owner=identity)
                self.rooms[room.code] = room
            else:
                room = self.rooms.get(room_code)
                if room is None:
                    await self.reject(writer, "room not found")
                    return
                if room.fingerprint != fingerprint:
                    await self.reject(writer, "game build mismatch")
                    return
                if identity in room.removed:
                    await self.reject(writer, "Removed from this room")
                    return
                previous = next((p for p in room.peers if p.identity == identity), None)
                if kind == b"R":
                    if previous is None or not secrets.compare_digest(previous.token, token):
                        await self.reject(writer, "Reconnect session expired")
                        return
                    # The old connection can remain half-open after a network drop.
                    previous.connected = False
                    previous.writer.close()
                elif previous is not None:
                    await self.reject(writer, "This trainer is already in the room")
                    return
                elif room.locked:
                    await self.reject(writer, "World is locked")
                    return
                if previous is None and len(room.peers) >= 2:
                    await self.reject(writer, "room is full")
                    return

            used_slots = {other.slot for other in room.peers}
            slot = previous.slot if previous else next(i for i in range(2) if i not in used_slots)
            peer = Peer(reader, writer, name, slot, room, identity=identity)
            if previous:
                if room.team_state:self.clear_team(room,6)
                peer.token = previous.token
                room.peers.remove(previous)
            existing = [p for p in room.peers if p.connected]
            room.peers.append(peer)
            code = room.code.encode("ascii")
            host_slot = next(p.slot for p in room.peers if p.identity == room.owner)
            peer.send(b"A", bytes([slot, len(code)]) + code + bytes([host_slot, room.locked]) + peer.token)
            for other in existing:
                peer.send(b"P", bytes([other.slot, len(other.name.encode())]) + other.name.encode() + other.identity)
                other.send(b"P", bytes([slot, len(name.encode())]) + name.encode() + identity)
            await writer.drain()

            while True:
                msg = await asyncio.wait_for(read_frame(reader), timeout=45)
                op, body = msg[:1], msg[1:]
                if op == b"S" and len(body) == 28 and body[27] <= 4:
                    peer.activity=body[27]
                    if body[22]&128 and peer.activity==1:
                        peer.pose=struct.unpack_from('<I4xiii',body);peer.pose_at=time.monotonic()
                    else:
                        peer.pose_at=0
                    # map id, sequence, x/y/z in centimetres, yaw, motion, and two signed
                    # stick axes. Only movement axes are mirrored into the native P2 actor.
                    for other in room.peers:
                        if other is not peer:
                            other.send(b"S", bytes([slot]) + body)
                elif op == b"T" and len(body) == 82 and body[0] <= 1 and body[1] <= 10:
                    # Gender, selected trainer style, and 80 bytes of model resource IDs.
                    for other in room.peers:
                        if other is not peer:
                            other.send(b"T", bytes([slot]) + body)
                elif op == b"E" and len(body) == 1 and body[0] <= 15:
                    for other in room.peers:
                        if other is not peer:
                            other.send(b"E", bytes([slot]) + body)
                elif op == b"Q" and len(body) == 4:
                    peer.send(b"q", body)
                elif op == b"U" and len(body)==5 and body[0]<=3:
                    self.team_action(peer,body[0],struct.unpack_from('<I',body,1)[0])
                elif op == b"L" and not body:
                    leaving = True
                    if peer.identity == room.owner:
                        self.close_room(room, "Host ended the session; continue playing solo")
                    break
                elif op == b"K" and len(body) == 1 and body[0] <= 1 and peer.identity == room.owner:
                    room.locked = bool(body[0])
                    for other in room.peers:
                        other.send(b"H", bytes([host_slot, room.locked]))
                elif op == b"D" and len(body) == 1 and peer.identity == room.owner and body[0] != slot:
                    target = next((p for p in room.peers if p.slot == body[0]), None)
                    if target:
                        if room.team_state:self.clear_team(room,6)
                        room.removed.add(target.identity)
                        await self.reject(target.writer, "Removed by the host")
                        target.writer.close()
                        room.peers.remove(target)
                        peer.send(b"L", bytes([target.slot]))
                else:
                    await self.reject(writer, "unsupported message")
                    break
                await writer.drain()
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, OSError, ValueError):
            pass
        finally:
            if peer:
                room = peer.room
                if peer in room.peers:
                    if room.team_state:
                        self.clear_team(room,6)
                    peer.connected = False
                    for other in list(room.peers):
                        other.send(b"L", bytes([peer.slot]))
                    if leaving:
                        room.peers.remove(peer)
                        if not room.peers:
                            self.rooms.pop(room.code, None)
                    else:
                        asyncio.create_task(self.expire(peer))
            self.connections = max(0, self.connections - 1)
            self.writers.discard(writer)
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass


async def health_server(relay: Relay, port: int) -> asyncio.AbstractServer:
    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await asyncio.wait_for(reader.readline(), timeout=3)
            ok = line.startswith(b"GET /health ")
            body = json.dumps(relay.health() if ok else {"error": "not found"}).encode()
            writer.write(
                b"HTTP/1.1 " + (b"200 OK" if ok else b"404 Not Found") + b"\r\n"
                + b"Content-Type: application/json\r\n"
                + b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                + b"Connection: close\r\n\r\n" + body
            )
            await writer.drain()
        except (asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            writer.close()

    return await asyncio.start_server(handle, "127.0.0.1", port)


async def main() -> None:
    relay = Relay()
    host = os.getenv("COOP_RELAY_HOST", "127.0.0.1")
    port = int(os.getenv("COOP_RELAY_PORT", "7381"))
    health_port = int(os.getenv("COOP_RELAY_HEALTH", "7382"))
    game = await asyncio.start_server(relay.handle, host, port, limit=MAX_FRAME + 2)
    health = await health_server(relay, health_port) if health_port else None
    print(f"co-op relay on {host}:{port}; health on {health_port or 'off'}", flush=True)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:  # Windows
            signal.signal(sig, lambda *_: loop.call_soon_threadsafe(stop.set))
    await stop.wait()
    game.close()
    if health:
        health.close()
    await game.wait_closed()
    if health:
        await health.wait_closed()
    for writer in list(relay.writers):
        writer.close()


if __name__ == "__main__":
    asyncio.run(main())
