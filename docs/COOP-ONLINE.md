# Online co-op prototype

This prototype relays player presence and emote packets between two matching game builds. It does not send or merge save data: each player continues to use their own local save directory. Story flags, inventory, Pokémon, battles, and progression are not shared.

## Run a private relay

Python 3.10 or later is required. From the repository root:

```powershell
$env:COOP_RELAY_HOST = "0.0.0.0"
python server/coop_relay.py
```

The relay listens on TCP port 7381 by default. Forward that port on the host firewall/router, or use a private VPN. The health endpoint is bound to `127.0.0.1:7382` and reports ephemeral room/player counts.

Start the host game with:

```powershell
$env:LARECOMP_COOP_SERVER = "relay-host.example:7381"
$env:LARECOMP_COOP_HOST = "1"
$env:LARECOMP_COOP_NAME = "Ash"
build\lasrecomp.exe --play
```

The first player's log prints a room code. On the second computer, use the same server and code:

```powershell
$env:LARECOMP_COOP_SERVER = "relay-host.example:7381"
$env:LARECOMP_COOP_ROOM = "LG-ABC234"
$env:LARECOMP_COOP_NAME = "Misty"
build\lasrecomp.exe --play
```

Both players need the same title and main module build. The runtime log reports the room join, remote player join/leave, and emotes. Once a peer joins, a small in-game presence indicator shows their name. F10 sends a wave emote.

For two instances on one PC, start them with distinct save directories and set `LARECOMP_GAMEPAD_ID` to the SDL device ID printed by `[input] detected gamepad`. That instance uses the selected device as P1 and does not claim a local P2 controller. SDL IDs are assigned when devices are detected, so check the log after connecting both controllers.

## Current prototype limits

- The client sends the local player position and receives the other player's position. Map/area is currently a placeholder, and the game has not yet been taught to spawn and animate a remote character. The indicator confirms the relay session, not an in-world avatar.
- Emotes are relayed and logged, but not yet displayed in the game world.
- Only two peers are accepted per room. Rooms exist only while at least one player is connected; no accounts or durable matchmaking are involved.
- The transport is plain TCP with no TLS or identity verification. Keep it on localhost or a trusted VPN while developing; do not expose it as a public service without adding transport security, abuse controls, and operational monitoring.

## Wire protocol

Each TCP message is a little-endian 16-bit byte length followed by a one-byte opcode and body (maximum 256 bytes). `C` creates a room and `J` joins one. The handshake body is protocol version (u8), title ID (u64 LE), main build ID (u64 LE), optional room-code length and ASCII code for `J`, then player-name length and UTF-8 name. `A` accepts with slot and room code; `P` announces a peer; `L` announces departure; `S` forwards a snapshot; `E` forwards an emote; `Q` is a keepalive. A snapshot body is area (u32), sequence (u32), x/y/z centimeters (i32 each), yaw (u16 turns), and motion (u8). `X` rejects a request with a short reason string.
