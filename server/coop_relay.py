#!/usr/bin/env python3
"""Small, ephemeral TCP relay for Let's Go Recomp's social co-op prototype.

The relay forwards presence, emotes and voluntary invitations. The opt-in battle prototype
exchanges a temporary serialized lead Pokemon and bounded turn decisions after consent.
The experimental mirror extension also exchanges the host's temporary battle party,
initial battle seed and committed commands so a guest can replay the native battle.
It never receives entire saves, inventory or story state. See docs/COOP-ONLINE.md.

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
    battle_id: int = 0
    battle_phase: int = 0
    battle_turn: int = 0
    battle_sequence: int = 0
    battle_deadline: float = 0
    battle_sent: bool = False
    battle_chunks: int = 0
    battle_choices: bytes = b""
    mirror_count: int = 0
    mirror_chunks: int = 0
    mirror_seed: bool = False
    mirror_sequences: list[int] = field(default_factory=lambda: [0, 0])
    mirror_aborted: bool = False
    mirror_digest: int = 0


class Relay:
    # B6 reasons: 0 finished, 1 cancelled, 2 peer disconnected, 3 offer expired.
    def clear_battle(self, room: Room, reason: int = 2, body: bytes = b"") -> None:
        if room.battle_id:
            body=body or b"\x06"+struct.pack('<QB',room.battle_id,reason)
            for p in room.peers:p.send(b"B",body)
        room.battle_id=room.battle_phase=room.battle_turn=room.battle_sequence=room.battle_chunks=0
        room.battle_deadline=0;room.battle_sent=False;room.battle_choices=b""
        room.mirror_count=room.mirror_chunks=room.mirror_digest=0;room.mirror_seed=room.mirror_aborted=False;room.mirror_sequences=[0,0]

    async def expire_battle_offer(self, room: Room, battle: int) -> None:
        await asyncio.sleep(45)
        if room.battle_id==battle and room.battle_phase==1:self.clear_battle(room,3)

    def battle_action(self, peer: Peer, body: bytes) -> None:
        if len(body)<9:return
        room=peer.room;op=body[0];battle=struct.unpack_from('<Q',body,1)[0]
        others=[p for p in room.peers if p is not peer and p.connected]
        if not battle or not others:return
        other=others[0];host=peer.identity==room.owner;now=time.monotonic()
        if op==0 and len(body)==13 and host and not room.battle_id:
            timeout,trainer=struct.unpack_from('<HH',body,9)
            if not 1000<=timeout<=60000 or not 1<=trainer<=601 or not self.team_near(peer,other):return
            room.battle_id=battle;room.battle_phase=1;room.battle_chunks=0
            asyncio.create_task(self.expire_battle_offer(room,battle))
        elif battle!=room.battle_id:return
        elif op==1 and len(body)==9 and not host and room.battle_phase==1:room.battle_phase=2
        elif op==2 and len(body)==140 and not host and room.battle_phase==2 and body[9]<2:
            bit=1<<body[9]
            if room.battle_chunks&bit:return
            room.battle_chunks|=bit
        elif op==3 and len(body)==52 and host and room.battle_phase==2 and room.battle_chunks==3:
            turn,sequence,timeout=struct.unpack_from('<IIH',body,9)
            if turn<=room.battle_turn or sequence<=room.battle_sequence or not 1<=timeout<=60000 or body[19]>=6:return
            room.battle_turn=turn;room.battle_sequence=sequence;room.battle_deadline=now+timeout/1000
            room.battle_sent=False;room.battle_choices=body
        elif op==4 and len(body)==22 and not host and room.battle_phase==2:
            turn,sequence=struct.unpack_from('<II',body,9)
            if room.battle_sent or now>=room.battle_deadline or turn!=room.battle_turn or sequence!=room.battle_sequence:return
            choice=room.battle_choices;index=body[18];target=body[21]
            if not choice or index>=4 or target>=16 or body[17]!=choice[19]:return
            move=struct.unpack_from('<H',body,19)[0]
            if not move or move!=struct.unpack_from('<H',choice,20+2*index)[0] or not choice[28+index] or not (struct.unpack_from('<H',choice,32+2*index)[0]&(1<<target)):return
            room.battle_sent=True
        elif op==5 and len(body)==18 and host and room.battle_phase==2:
            if struct.unpack_from('<II',body,9)!=(room.battle_turn,room.battle_sequence) or body[17] not in (1,2):return
            room.battle_deadline=0;room.battle_sent=True
        elif op==6 and (len(body)==10 and body[9]<=3 or len(body)==18 and host and body[9]==0):
            # The host's final command counts reach the guest unchanged.
            self.clear_battle(room,body=body);return
        elif op==10 and len(body)==10 and body[9] and room.battle_phase==2 and room.mirror_count:
            if room.mirror_aborted:return
            room.mirror_aborted=True
        elif op==11 and len(body)==21 and host and room.battle_phase==2 and room.mirror_seed and not room.mirror_aborted:
            digest_round=struct.unpack_from('<I',body,9)[0]
            if digest_round<=room.mirror_digest:return
            room.mirror_digest=digest_round
        elif room.mirror_aborted and op in (7,8,9):return
        elif op==7 and len(body)==141 and host and room.battle_phase==2 and room.battle_chunks==3:
            count,index=body[9:11]
            if not 1<=count<=6 or index>=2*count or room.mirror_seed:return
            if room.mirror_count and room.mirror_count!=count:return
            if room.mirror_chunks&(1<<index):return
            room.mirror_count=count;room.mirror_chunks|=1<<index
        elif op==8 and len(body)==17 and host and room.battle_phase==2:
            if room.mirror_seed or not room.mirror_count or room.mirror_chunks!=(1<<(2*room.mirror_count))-1:return
            room.mirror_seed=True
        elif op==9 and len(body)==24 and host and room.battle_phase==2 and room.mirror_seed:
            turn,sequence=struct.unpack_from('<II',body,9);trainer,party,index=body[17:20];move=struct.unpack_from('<H',body,20)[0];target,ai=body[22:24]
            if trainer not in (0,2) or not turn or not sequence or party>=6 or index>=4 or not 1<=move<=742 or target>=4 or ai>1 or (ai and trainer!=2):return
            actor=trainer//2
            if sequence!=room.mirror_sequences[actor]+1:return
            if trainer==2:
                choice=room.battle_choices
                if not choice or (turn,sequence)!=(room.battle_turn,room.battle_sequence) or party!=choice[19]:return
                if move!=struct.unpack_from('<H',choice,20+2*index)[0] or not choice[28+index] or not (struct.unpack_from('<H',choice,32+2*index)[0]&(1<<target)):return
            room.mirror_sequences[actor]=sequence
        else:return
        other.send(b"B",body)

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
                elif op == b"B":
                    self.battle_action(peer,body)
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
                        if room.battle_id:self.clear_battle(room)
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
                    if room.battle_id:self.clear_battle(room)
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
