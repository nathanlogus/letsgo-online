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
  COOP_RELAY_HEALTH_HOST  its bind address (default 127.0.0.1; the Compose network in a container)
  COOP_RELAY_ADMIN_TOKEN  enables GET /rooms and POST /rooms/<id>/close on the health port for the account
                        service's dashboard (Authorization: Bearer <token>); unset: those answer 404
  COOP_RELAY_MAX_ROOMS  maximum rooms in memory (default 100)
  COOP_RELAY_TRADE_LOG  file that keeps trade decisions across relay restarts (default: memory only)
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import os
import secrets
import signal
import socket
import struct
import time
from dataclasses import dataclass, field

PROTO = 11  # 6: team-up trainer battles (B) in normal play; 7: whole guest party, switches and replacements; 8: the turn digest (B11) skips battle Pokemon bytes the game never sets; 9: the offer (B0) carries the host's hold; 10: wild team-ups (B0 trainer 0x8000 | species, B7 chunks 12-13 the wild Pokemon), the seed (B8) carries the host's own-Pokemon mask and language; 11: meet up (M), trades (W), practice battles and spectating (V, O, B0 0x4000, slot-1 B9/B6), story scenes (Y); mixed versions are refused and play solo
RECONNECT_GRACE = 30
SCENE_SECONDS = 60  # a Shared Adventure ready prompt
VERSUS_START = 20  # after a practice battle request is accepted, the room's host offers the battle (B0 0x4000) within this
TEAM_INVITE_SECONDS = 45  # Leave time to navigate Communicate and return to the overworld.
TRADE_KEEP = 7 * 86400  # a trade decision answers a client in doubt this long
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


def room_id(code: str) -> str:
    return hashlib.sha256(code.encode()).hexdigest()[:12]


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
    adventure: int = 0  # Y0: 0 Social, 1 Shared, 2 Shared with this session's consent

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
    meet_state: int = 0  # meet-up request (M): 0 none, 1 pending; 2 (accepted) is only sent
    meet_id: int = 0
    meet_from: int = 255
    meet_until: float = 0
    # Practice battle request (V), as M; until versus_start the room's host may offer the agreed battle. watcher: the
    # slot watching the other player's battle (O), 255 none.
    versus_state: int = 0
    versus_id: int = 0
    versus_from: int = 255
    versus_until: float = 0
    versus_start: float = 0
    battle_pvp: bool = False
    watcher: int = 255
    battle_id: int = 0
    battle_phase: int = 0
    battle_turn: int = 0
    battle_sequence: int = 0
    battle_deadline: float = 0
    battle_sent: bool = False
    battle_chunks: int = 0
    battle_count: int = 0
    battle_choices: bytes = b""
    battle_wild: bool = False
    mirror_count: int = 0
    mirror_chunks: int = 0
    mirror_seed: bool = False
    mirror_sequences: list[int] = field(default_factory=lambda: [0, 0])
    mirror_aborted: bool = False
    mirror_digest: int = 0
    # Trade (W): 1 requested, 2 open, 3 both confirmed (waiting for both "prepared"). Per slot: offer [rev, bytes,
    # chunks], confirmation (own rev, other rev), prepared digest.
    trade_id: int = 0
    trade_phase: int = 0
    trade_from: int = 255
    trade_offers: dict[int, list] = field(default_factory=dict)
    trade_confirms: dict[int, tuple[int, int]] = field(default_factory=dict)
    trade_prepared: dict[int, int] = field(default_factory=dict)
    # Shared Adventure (Y): the open scene offer, its initiator's slot and the partner it was sent to.
    scene_id: int = 0
    scene_from: int = 255
    scene_to: bytes = b""
    scene_offer: bytes = b""


class Relay:
    # B6 reasons: 0 finished, 1 cancelled, 2 peer disconnected, 3 offer expired.
    def clear_battle(self, room: Room, reason: int = 2, body: bytes = b"") -> None:
        if room.battle_id:
            body=body or b"\x06"+struct.pack('<QB',room.battle_id,reason)
            for p in room.peers:p.send(b"B",body)
        room.battle_id=room.battle_phase=room.battle_turn=room.battle_sequence=room.battle_chunks=room.battle_count=0
        room.battle_deadline=0;room.battle_sent=False;room.battle_choices=b"";room.battle_wild=False;room.battle_pvp=False
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
        if op==0 and len(body)==15 and host and not room.battle_id:
            timeout,trainer,hold=struct.unpack_from('<HHH',body,9)
            wild=0x8001<=trainer<=0x8000+809;pvp=trainer==0x4000
            if not 1000<=timeout<=60000 or not (1<=trainer<=601 or wild or pvp) or not 1000<=hold<=45000:return
            # A practice battle needs the agreement (V) just before; a team-up needs the tag, nearby.
            if pvp:
                if now>=room.versus_start or room.watcher!=255:return
                room.versus_start=0
            elif room.team_state!=2 or not self.team_near(peer,other):return
            room.battle_id=battle;room.battle_phase=1;room.battle_chunks=0;room.battle_wild=wild;room.battle_pvp=pvp
            asyncio.create_task(self.expire_battle_offer(room,battle))
        elif battle!=room.battle_id:return
        elif op==1 and len(body)==9 and not host and room.battle_phase==1:room.battle_phase=2
        elif op==2 and len(body)==141 and not host and room.battle_phase==2 and 1<=body[9]<=6 and body[10]<2*body[9]:
            # The guest's whole party, two chunks per Pokemon.
            bit=1<<body[10]
            if room.battle_chunks&bit or (room.battle_count and room.battle_count!=body[9]):return
            room.battle_count=body[9];room.battle_chunks|=bit
        elif op==3 and len(body)==54 and host and room.battle_phase==2 and self.party_done(room):
            turn,sequence,timeout=struct.unpack_from('<IIH',body,9)
            if turn<=room.battle_turn or sequence<=room.battle_sequence or not 1<=timeout<=60000 or body[19]>=6:return
            room.battle_turn=turn;room.battle_sequence=sequence;room.battle_deadline=now+timeout/1000
            room.battle_sent=False;room.battle_choices=body
        elif op==4 and len(body)==23 and not host and room.battle_phase==2:
            turn,sequence=struct.unpack_from('<II',body,9)
            if room.battle_sent or now>=room.battle_deadline or turn!=room.battle_turn or sequence!=room.battle_sequence:return
            choice=room.battle_choices;index=body[18];target=body[21];switch=body[22]
            if not choice or index>=4 or target>=16 or body[17]!=choice[19]:return
            if switch:
                if switch>6 or not choice[53]&(1<<(switch-1)):return
            else:
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
        elif op==7 and len(body)==141 and host and room.battle_phase==2 and self.party_done(room):
            count,index=body[9:11]
            # (a wild team-up's wild Pokemon: chunks 12 and 13)
            if not 1<=count<=6 or (index>=2*count and not (room.battle_wild and index in (12,13))) or room.mirror_seed:return
            if room.mirror_count and room.mirror_count!=count:return
            if room.mirror_chunks&(1<<index):return
            room.mirror_count=count;room.mirror_chunks|=1<<index
        elif op==8 and len(body)==19 and host and room.battle_phase==2:
            if room.mirror_seed or not room.mirror_count or room.mirror_chunks!=((1<<(2*room.mirror_count))-1)|(0x3000 if room.battle_wild else 0):return
            room.mirror_seed=True
        elif op==9 and len(body)==30 and host and room.battle_phase==2 and room.mirror_seed:
            # Any native decision as its command word (low nibble 1 fight, 3 change); request kind 2 action, 4/5 replacement.
            turn,sequence=struct.unpack_from('<II',body,9);trainer,party,index=body[17:20];move=struct.unpack_from('<H',body,20)[0];target,ai,kind,count=body[22:26]
            packet=struct.unpack_from('<I',body,26)[0]
            remote=1 if room.battle_pvp else 2  # the guest's trainer slot: the opponent in a practice battle, else the ally
            if trainer not in (0,remote) or not turn or not sequence or party>=6 or ai>1 or (ai and trainer!=remote) or kind not in (2,4,5) or count>10:return
            actor=int(trainer!=0)
            if sequence!=room.mirror_sequences[actor]+1:return
            if trainer==remote and not ai:
                # The guest's own decision: it must be one the host offered it.
                choice=room.battle_choices
                if not choice or (turn,sequence)!=(room.battle_turn,room.battle_sequence) or party!=choice[19]:return
                if packet&15==3:
                    if not choice[53]&(1<<((packet>>7)&7)):return
                elif packet&15!=1 or index>=4 or move!=struct.unpack_from('<H',choice,20+2*index)[0] or not choice[28+index] or not (struct.unpack_from('<H',choice,32+2*index)[0]&(1<<target)):return
            room.mirror_sequences[actor]=sequence
        else:return
        other.send(b"B",body)

    @staticmethod
    def party_done(room: Room) -> bool:
        return room.battle_count>0 and room.battle_chunks==(1<<(2*room.battle_count))-1

    # Trades: the relay is the commit point. A trade commits only when both sides said "prepared" (each wrote its
    # durable record first) for the offers both confirmed; anything earlier (cancel, disconnect, timeout) aborts it.
    # Decisions are kept (TRADE_KEEP seconds, and in COOP_RELAY_TRADE_LOG) so a client in doubt can ask (W7).
    # W5 reasons: 1 cancelled, 2 disconnected, 3 expired, 4 prepare timeout, 5 digest mismatch, 6 refused.
    def trade_decide(self, room: Room, decision: int, reason: int) -> None:
        tid=room.trade_id
        if not tid:return
        now=time.time();self.trade_decisions[tid]=(decision,reason,now)
        for k in [k for k,v in self.trade_decisions.items() if now-v[2]>TRADE_KEEP]:del self.trade_decisions[k]
        log=os.getenv("COOP_RELAY_TRADE_LOG")
        if log:
            with open(log,"a") as f:f.write(f"{tid:016x} {decision} {reason} {int(now)}\n");f.flush();os.fsync(f.fileno())
        for p in room.peers:p.send(b"W",b"\x05"+struct.pack('<QBB',tid,decision,reason))
        room.trade_id=room.trade_phase=0;room.trade_from=255;room.trade_offers={};room.trade_confirms={};room.trade_prepared={}

    async def expire_trade(self, room: Room, tid: int, phase: int, seconds: float, reason: int) -> None:
        await asyncio.sleep(seconds)
        if room.trade_id==tid and room.trade_phase==phase:self.trade_decide(room,2,reason)

    @staticmethod
    def fnv(data: bytes) -> int:
        h=0xcbf29ce484222645
        for b in data:h=((h^b)*0x100000001b3)&0xffffffffffffffff
        return h

    def trade_action(self, peer: Peer, body: bytes) -> None:
        if len(body)<9:return
        room=peer.room;op=body[0];tid=struct.unpack_from('<Q',body,1)[0]
        other=next((p for p in room.peers if p is not peer and p.connected),None)
        if not tid:return
        if op==7 and len(body)==9:
            d=self.trade_decisions.get(tid)
            peer.send(b"W",b"\x05"+struct.pack('<QBB',tid,d[0] if d else 0,d[1] if d else 0));return
        if op==8 and len(body)==11 and body[9]<=3 and body[10]<=1:
            if other:other.send(b"W",body)
            return
        if op==0 and len(body)==9:
            if (room.trade_id or other is None or room.battle_id or tid in self.trade_decisions or
                    not (room.team_state==2 or self.team_near(peer,other))):
                peer.send(b"W",b"\x05"+struct.pack('<QBB',tid,2,6));return
            room.trade_id=tid;room.trade_phase=1;room.trade_from=peer.slot
            asyncio.create_task(self.expire_trade(room,tid,1,TEAM_INVITE_SECONDS,3))
        elif tid!=room.trade_id or other is None:return
        elif op==1 and len(body)==9 and room.trade_phase==1 and peer.slot!=room.trade_from:room.trade_phase=2
        elif op==2 and len(body)==144 and room.trade_phase==2 and body[13]<2:
            rev=struct.unpack_from('<I',body,9)[0];offer=room.trade_offers.setdefault(peer.slot,[0,bytearray(260),0])
            if rev<offer[0]:return
            if rev>offer[0]:offer[0]=rev;offer[2]=0
            offer[1][130*body[13]:130*body[13]+130]=body[14:144];offer[2]|=1<<body[13]
            room.trade_confirms={}  # any offer change clears both confirmations
        elif op==3 and len(body)==17 and room.trade_phase==2:
            mine,theirs=room.trade_offers.get(peer.slot),room.trade_offers.get(other.slot)
            if not mine or not theirs or mine[2]!=3 or theirs[2]!=3 or struct.unpack_from('<II',body,9)!=(mine[0],theirs[0]):return
            room.trade_confirms[peer.slot]=(mine[0],theirs[0])
            if len(room.trade_confirms)==2:
                room.trade_phase=3;asyncio.create_task(self.expire_trade(room,tid,3,30,4))
        elif op==4 and len(body)==17 and room.trade_phase==2:
            # (it prepared on confirmations an offer change has cleared here: the two sides disagree)
            self.trade_decide(room,2,5);return
        elif op==4 and len(body)==17 and room.trade_phase==3:
            digest=(self.fnv(bytes(room.trade_offers[peer.slot][1]))+self.fnv(bytes(room.trade_offers[other.slot][1])))&0xffffffffffffffff
            if struct.unpack_from('<Q',body,9)[0]!=digest:self.trade_decide(room,2,5);return
            room.trade_prepared[peer.slot]=digest
            if len(room.trade_prepared)==2:self.trade_decide(room,1,0)
            return
        elif op==6 and len(body)==9:self.trade_decide(room,2,1);return
        else:return
        other.send(b"W",body)

    # Shared Adventure scenes: a gated forwarder. An offer reaches the partner only when both players are in Shared
    # mode with this session's consent (Y0 mode 2), and for "Nearby" only tagged up or within 30 m. The answer is the
    # partner's own, final decision; the relay keeps the offer through the reconnect grace and resends it.
    # Y4 reasons: 1 not both sharing, 2 not nearby, 3 another offer is open, 4 no partner, 5 expired, 6 initiator gone,
    # 7 no such offer.
    def scene_close(self, room: Room, reason: int) -> None:
        if room.scene_id:
            for p in room.peers:p.send(b"Y",b"\x04"+struct.pack('<QB',room.scene_id,reason))
        room.scene_id=0;room.scene_from=255;room.scene_to=b"";room.scene_offer=b""

    async def expire_scene(self, room: Room, sid: int) -> None:
        await asyncio.sleep(SCENE_SECONDS)
        if room.scene_id==sid:self.scene_close(room,5)

    async def scene_grace(self, room: Room, sid: int, identity: bytes) -> None:
        await asyncio.sleep(RECONNECT_GRACE)
        if room.scene_id==sid and not any(p.identity==identity and p.connected for p in room.peers):self.scene_close(room,6)

    def scene_action(self, peer: Peer, body: bytes) -> None:
        room=peer.room
        other=next((p for p in room.peers if p is not peer and p.connected),None)
        if len(body)==2 and body[0]==0 and body[1]<=2:
            peer.adventure=body[1]
            for p in room.peers:
                if p is not peer:p.send(b"Y",body)
            return
        if len(body)<10:return
        op=body[0];sid=struct.unpack_from('<Q',body,1)[0]
        if not sid:return
        if op==1 and len(body)==12 and body[11]<=1:
            reason=(3 if room.scene_id else 4 if other is None else 1 if peer.adventure!=2 or other.adventure!=2 else
                    2 if body[11] and not (room.team_state==2 or self.team_near(peer,other)) else 0)
            if reason:peer.send(b"Y",b"\x04"+struct.pack('<QB',sid,reason));return
            room.scene_id=sid;room.scene_from=peer.slot;room.scene_to=other.identity;room.scene_offer=body
            asyncio.create_task(self.expire_scene(room,sid))
        elif sid!=room.scene_id:
            if op==2 and len(body)==10:peer.send(b"Y",b"\x04"+struct.pack('<QB',sid,7))
            return
        elif op==2 and len(body)==10 and 1<=body[9]<=5 and peer.slot!=room.scene_from:
            room.scene_id=0;room.scene_from=255;room.scene_to=b"";room.scene_offer=b""
        elif op==3 and len(body)==10 and body[9] in (1,2) and peer.slot==room.scene_from:
            room.scene_id=0;room.scene_from=255;room.scene_to=b"";room.scene_offer=b""
        else:return
        if other:other.send(b"Y",body)

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

    # Meet-up (M): the U invitation's commands, states and reasons, without the distance rule (they are apart). On
    # accept both get state 2 once and the room is free for the next request; the requester's game flies.
    def meet_status(self, room: Room, state: int, reason: int = 0, target: Peer | None = None) -> None:
        remaining=max(0,int((room.meet_until-time.monotonic())*1000)) if state==1 else 0
        body=struct.pack('<BBBII',state,room.meet_from,reason,room.meet_id,remaining)
        for peer in ([target] if target else room.peers):
            peer.send(b'M',body)

    def clear_meet(self, room: Room, reason: int) -> None:
        room.meet_state=0;room.meet_until=0;self.meet_status(room,0,reason);room.meet_from=255

    async def expire_meet(self, room: Room, request: int) -> None:
        await asyncio.sleep(TEAM_INVITE_SECONDS)
        if room.meet_state==1 and room.meet_id==request:
            self.clear_meet(room,5)

    @staticmethod
    def meet_ready(peer: Peer) -> bool:
        return peer.activity==1 and peer.pose is not None and time.monotonic()-peer.pose_at<1

    def meet_action(self, peer: Peer, command: int, request: int) -> None:
        room=peer.room
        other=next((p for p in room.peers if p is not peer and p.connected),None)
        if command==0:
            if room.meet_state or other is None:
                self.meet_status(room,room.meet_state,3,peer);return
            if not (self.meet_ready(peer) and self.meet_ready(other)):
                self.meet_status(room,0,4,peer);return
            room.meet_id=secrets.randbits(32) or 1
            room.meet_state=1;room.meet_from=peer.slot;room.meet_until=time.monotonic()+TEAM_INVITE_SECONDS
            self.meet_status(room,1)
            asyncio.create_task(self.expire_meet(room,room.meet_id))
        elif request!=room.meet_id or room.meet_state!=1:
            self.meet_status(room,room.meet_state,7,peer)
        elif command==1 and peer.slot!=room.meet_from and other is not None:
            if time.monotonic()>=room.meet_until:
                self.clear_meet(room,5);return
            if not (self.meet_ready(peer) and self.meet_ready(other)):
                self.meet_status(room,1,4,peer);return
            room.meet_state=0;room.meet_until=0;self.meet_status(room,2);room.meet_from=255
        elif command==2 and peer.slot!=room.meet_from:
            self.clear_meet(room,1)
        elif command==3 and peer.slot==room.meet_from:
            self.clear_meet(room,2)
        else:
            self.meet_status(room,room.meet_state,7,peer)

    # Practice battles (V): the meet-up's commands, states and reasons. Both players must be on the overworld, with no
    # battle, trade or watch running; on accept both get state 2 once, and the room's host may offer the battle.
    def versus_status(self, room: Room, state: int, reason: int = 0, target: Peer | None = None) -> None:
        remaining=max(0,int((room.versus_until-time.monotonic())*1000)) if state==1 else 0
        body=struct.pack('<BBBII',state,room.versus_from,reason,room.versus_id,remaining)
        for peer in ([target] if target else room.peers):peer.send(b'V',body)

    def clear_versus(self, room: Room, reason: int) -> None:
        room.versus_state=0;room.versus_until=0;self.versus_status(room,0,reason);room.versus_from=255

    async def expire_versus(self, room: Room, request: int) -> None:
        await asyncio.sleep(TEAM_INVITE_SECONDS)
        if room.versus_state==1 and room.versus_id==request:self.clear_versus(room,5)

    def versus_free(self, room: Room, peer: Peer, other: Peer) -> bool:
        return self.meet_ready(peer) and self.meet_ready(other) and not room.battle_id and not room.trade_id and room.watcher==255

    def versus_action(self, peer: Peer, command: int, request: int) -> None:
        room=peer.room
        other=next((p for p in room.peers if p is not peer and p.connected),None)
        if command==0:
            if room.versus_state or other is None:
                self.versus_status(room,room.versus_state,3,peer);return
            if not self.versus_free(room,peer,other):
                self.versus_status(room,0,4,peer);return
            room.versus_id=secrets.randbits(32) or 1
            room.versus_state=1;room.versus_from=peer.slot;room.versus_until=time.monotonic()+TEAM_INVITE_SECONDS
            self.versus_status(room,1)
            asyncio.create_task(self.expire_versus(room,room.versus_id))
        elif request!=room.versus_id or room.versus_state!=1:
            self.versus_status(room,room.versus_state,7,peer)
        elif command==1 and peer.slot!=room.versus_from and other is not None:
            if time.monotonic()>=room.versus_until:
                self.clear_versus(room,5);return
            if not self.versus_free(room,peer,other):
                self.versus_status(room,1,4,peer);return
            room.versus_state=0;room.versus_until=0;room.versus_start=time.monotonic()+VERSUS_START
            self.versus_status(room,2);room.versus_from=255
        elif command==2 and peer.slot!=room.versus_from:
            self.clear_versus(room,1)
        elif command==3 and peer.slot==room.versus_from:
            self.clear_versus(room,2)
        else:
            self.versus_status(room,room.versus_state,7,peer)

    # Spectating (O): a player watches the other's battle as a summary the battler sends. 0 watch (the other is in a
    # battle, the watcher is not, and no battle link runs), 1 stop, 2 + 30-byte summary: from the battler to the watcher
    # only. Status 0x10: watching, watcher slot, reason (1 stopped, 2 battle over, 3 refused, 4 disconnected). The relay
    # drops the watcher's battle messages (B): a spectator can't affect any battle.
    def watch_status(self, room: Room, reason: int = 0, target: Peer | None = None) -> None:
        body=bytes([0x10,int(room.watcher!=255),room.watcher,reason])
        for peer in ([target] if target else room.peers):peer.send(b'O',body)

    def clear_watch(self, room: Room, reason: int) -> None:
        if room.watcher!=255:
            room.watcher=255;self.watch_status(room,reason)

    def watch_action(self, peer: Peer, body: bytes) -> None:
        room=peer.room
        other=next((p for p in room.peers if p is not peer and p.connected),None)
        if body==b'\x00':
            if room.watcher!=255 or other is None or other.activity!=3 or peer.activity==3 or room.battle_id:
                self.watch_status(room,3,peer);return
            room.watcher=peer.slot;self.watch_status(room)
        elif body==b'\x01' and room.watcher==peer.slot:
            self.clear_watch(room,1)
        elif len(body)==31 and body[0]==2 and other is not None and room.watcher==other.slot and peer.activity==3:
            other.send(b'O',body)

    def watch_activity(self, room: Room, peer: Peer) -> None:
        # The battler back on the overworld ends the watch; so does the watcher entering a battle.
        if peer.slot==room.watcher:
            if peer.activity==3:self.clear_watch(room,1)
        elif peer.activity==1:
            self.clear_watch(room,2)

    def versus_reset(self, room: Room) -> None:
        if room.versus_state:self.clear_versus(room,6)
        room.versus_start=0;self.clear_watch(room,4)

    def __init__(self) -> None:
        self.rooms: dict[str, Room] = {}
        self.connections = 0
        self.started = time.monotonic()
        self.writers: set[asyncio.StreamWriter] = set()
        self.trade_decisions: dict[int, tuple[int, int, float]] = {}
        log = os.getenv("COOP_RELAY_TRADE_LOG")
        if log and os.path.exists(log):
            with open(log) as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 4:
                        self.trade_decisions[int(parts[0], 16)] = (int(parts[1]), int(parts[2]), float(parts[3]))

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

    def rooms_view(self) -> list[dict[str, object]]:
        """The dashboard's view: a room is named by a hash of its code (the code is the invitation), never the code."""
        instant = time.monotonic()
        return [{
            "id": room_id(room.code),
            "players": [{"name": p.name, "slot": p.slot, "connected": p.connected, "host": p.identity == room.owner,
                         "seconds": int(instant - p.joined_at)} for p in room.peers],
            "locked": room.locked, "team": room.team_state, "battle": room.battle_phase,
            "seconds": int(instant - min((p.joined_at for p in room.peers), default=instant)),
        } for room in self.rooms.values()]

    def close_by_id(self, rid: str) -> bool:
        for room in list(self.rooms.values()):
            if room_id(room.code) == rid:
                self.close_room(room, "The server closed this session; continue playing solo")
                return True
        return False

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
                if room.meet_state:self.clear_meet(room,6)
                self.versus_reset(room)
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
                if other.adventure:peer.send(b"Y",bytes([0,other.adventure]))
            if room.scene_id and identity==room.scene_to:peer.send(b"Y",room.scene_offer)  # (back within the grace)
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
                    if room.watcher!=255:self.watch_activity(room,peer)
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
                elif op == b"E" and len(body) == 1 and 1 <= body[0] <= 63:  # emote IDs; clients ignore ones they do not list
                    for other in room.peers:
                        if other is not peer:
                            other.send(b"E", bytes([slot]) + body)
                elif op == b"Q" and len(body) == 4:
                    peer.send(b"q", body)
                elif op == b"B":
                    if room.watcher!=peer.slot:self.battle_action(peer,body)
                elif op == b"W":
                    self.trade_action(peer,body)
                elif op == b"Y":
                    self.scene_action(peer,body)
                elif op == b"M" and len(body)==5 and body[0]<=3:
                    self.meet_action(peer,body[0],struct.unpack_from('<I',body,1)[0])
                elif op == b"V" and len(body)==5 and body[0]<=3:
                    self.versus_action(peer,body[0],struct.unpack_from('<I',body,1)[0])
                elif op == b"O" and len(body) in (1,31):
                    self.watch_action(peer,body)
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
                        if room.meet_state:self.clear_meet(room,6)
                        self.versus_reset(room)
                        self.trade_decide(room,2,2)
                        self.scene_close(room,6)
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
                    self.trade_decide(room,2,2)
                    if room.team_state:
                        self.clear_team(room,6)
                    if room.meet_state:self.clear_meet(room,6)
                    self.versus_reset(room)
                    if leaving:self.scene_close(room,6)
                    elif room.scene_id and room.scene_from==peer.slot:asyncio.create_task(self.scene_grace(room,room.scene_id,peer.identity))
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


def health_answer(relay: Relay, line: bytes, headers: dict[bytes, bytes], token: str) -> tuple[int, object]:
    if line.startswith(b"GET /health "):
        return 200, relay.health()
    parts = line.split(b" ")
    if not token or len(parts) < 2 or not parts[1].startswith(b"/rooms"):
        return 404, {"error": "not found"}
    given = headers.get(b"authorization", b"")
    if not hmac.compare_digest(given, b"Bearer " + token.encode()):
        return 401, {"error": "unauthorized"}
    if parts[0] == b"GET" and parts[1] == b"/rooms":
        return 200, {"rooms": relay.rooms_view(), "summary": relay.health()}
    path = parts[1].decode("ascii", "replace").split("/")
    if parts[0] == b"POST" and len(path) == 4 and path[3] == "close" and len(path[2]) == 12:
        return (200, {"closed": path[2]}) if relay.close_by_id(path[2]) else (404, {"error": "no such room"})
    return 404, {"error": "not found"}


async def health_server(relay: Relay, port: int, host: str = "127.0.0.1", token: str = "") -> asyncio.AbstractServer:
    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await asyncio.wait_for(reader.readline(), timeout=3)
            headers: dict[bytes, bytes] = {}
            for _ in range(32):
                h = await asyncio.wait_for(reader.readline(), timeout=3)
                if h in (b"\r\n", b"\n", b""):
                    break
                k, _, v = h.partition(b":")
                headers[k.strip().lower()] = v.strip()
            status, answer = health_answer(relay, line, headers, token)
            body = json.dumps(answer).encode()
            reason = {200: b"200 OK", 401: b"401 Unauthorized", 404: b"404 Not Found"}[status]
            writer.write(
                b"HTTP/1.1 " + reason + b"\r\n"
                + b"Content-Type: application/json\r\n"
                + b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                + b"Connection: close\r\n\r\n" + body
            )
            await writer.drain()
        except (asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            writer.close()

    return await asyncio.start_server(handle, host, port, limit=4096)


async def main() -> None:
    relay = Relay()
    host = os.getenv("COOP_RELAY_HOST", "127.0.0.1")
    port = int(os.getenv("COOP_RELAY_PORT", "7381"))
    health_port = int(os.getenv("COOP_RELAY_HEALTH", "7382"))
    game = await asyncio.start_server(relay.handle, host, port, limit=MAX_FRAME + 2)
    health = await health_server(relay, health_port, os.getenv("COOP_RELAY_HEALTH_HOST", "127.0.0.1"),
                                 os.getenv("COOP_RELAY_ADMIN_TOKEN", "")) if health_port else None
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
