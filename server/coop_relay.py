#!/usr/bin/env python3
"""Small, ephemeral TCP relay for Let's Go Recomp's social co-op prototype.

The relay only forwards presence snapshots and emotes. It never receives save data,
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
import struct
import time
from dataclasses import dataclass, field

PROTO = 1
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
    name = name.strip()[:24]
    return name or "Trainer"


@dataclass(eq=False)
class Peer:
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    name: str
    slot: int
    room: "Room"
    joined_at: float = field(default_factory=time.monotonic)

    def send(self, kind: bytes, body: bytes = b"") -> None:
        if not self.writer.is_closing():
            self.writer.write(frame(kind, body))


@dataclass
class Room:
    code: str
    fingerprint: tuple[int, int]
    peers: list[Peer] = field(default_factory=list)


class Relay:
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
            "rooms": len(self.rooms),
            "players": sum(len(room.peers) for room in self.rooms.values()),
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

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer: Peer | None = None
        self.connections += 1
        self.writers.add(writer)
        try:
            # Connects that never identify a room are discarded quickly.
            hello = await asyncio.wait_for(read_frame(reader), timeout=10)
            if len(hello) < 20 or hello[0:1] not in (b"C", b"J"):
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
            if kind == b"J":
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
            if not 1 <= nname <= 48 or off + nname != len(hello):
                await self.reject(writer, "bad player name")
                return
            name = clean_name(hello[off:])

            if kind == b"C":
                if len(self.rooms) >= MAX_ROOMS:
                    await self.reject(writer, "relay is full")
                    return
                room = Room(self.new_code(), fingerprint)
                self.rooms[room.code] = room
            else:
                room = self.rooms.get(room_code)
                if room is None:
                    await self.reject(writer, "room not found")
                    return
                if room.fingerprint != fingerprint:
                    await self.reject(writer, "game build mismatch")
                    return
                if len(room.peers) >= 2:
                    await self.reject(writer, "room is full")
                    return

            slot = 0 if not room.peers else 1
            peer = Peer(reader, writer, name, slot, room)
            existing = list(room.peers)
            room.peers.append(peer)
            code = room.code.encode("ascii")
            peer.send(b"A", bytes([slot, len(code)]) + code)
            for other in existing:
                peer.send(b"P", bytes([other.slot, len(other.name.encode())]) + other.name.encode())
                other.send(b"P", bytes([slot, len(name.encode())]) + name.encode())
            await writer.drain()

            while True:
                msg = await asyncio.wait_for(read_frame(reader), timeout=45)
                op, body = msg[:1], msg[1:]
                if op == b"S" and len(body) == 23:
                    # map id, sequence, x/y/z in centimetres, yaw in 1/65536 turns, motion state
                    for other in room.peers:
                        if other is not peer:
                            other.send(b"S", bytes([slot]) + body)
                elif op == b"E" and len(body) == 1 and body[0] <= 15:
                    for other in room.peers:
                        if other is not peer:
                            other.send(b"E", bytes([slot]) + body)
                elif op == b"Q" and len(body) == 4:
                    peer.send(b"q", body)
                elif op == b"L" and not body:
                    break
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
                    room.peers.remove(peer)
                    for other in list(room.peers):
                        other.send(b"L", bytes([peer.slot]))
                    if not room.peers:
                        self.rooms.pop(room.code, None)
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
