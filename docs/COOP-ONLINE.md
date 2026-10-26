# Online co-op prototype

This prototype relays player presence and emote packets between two matching game builds. It does not send or merge save data: each player continues to use their own local save directory. Story flags, inventory, Pokémon, battles, and progression are not shared.

## Upstream integration and social tag-ups

Upstream `raulzrrs/letsgo-recomp` was merged through `60b8bdf` (0.7.3). This includes renderer/optical-flow optimizations, SSE4.2 game-code support, setup validation/rebuild fixes and the native CATCH implementation. The local Eevee classic battle-Bag catching mode remains the default; upstream CATCH/GO-return hooks run in the alternate mode (`LARECOMP_WILDBATTLE_CLASSIC=0`) to avoid two battle implementations modifying the same callbacks. Frame generation method is also exposed in native PC Options.

Protocol **5** adds voluntary social tag-ups. Both clients and the relay must be updated together. Friends and Communicate → Play With Others → Players → remote trainer → Team up offer an invitation, accept/decline, cancellation and end-tag controls. Invitations expire after 45 seconds. Both trainers must have fresh poses, be exploring the same area and be within 30 metres; the relay checks these conditions again on acceptance. Disconnect, removal and resumed sessions cancel the tag. The HUD identifies incoming invitations and an accepted social tag. Battles, rewards and saves remain individual; synchronized battles are not implemented by this invitation foundation.

Native Communicate queues invite/accept until the player returns to the overworld. A fresh snapshot is sent before that action, avoiding a race against the menu's busy state. Native connection pages now provide retry/cancel actions. Outfit changes automatically cycle the virtual P2 controller and rebuild its model with the new resource block.

For Windows source builds, `make -j8 build/LetsGoRecomp.exe` builds the executable whose name the game DLL imports. `build/lasrecomp.exe` remains a compatibility target. After an upstream generator/header change, `build/LetsGoRecomp.exe --rebuild-game` rebuilds the game library without opening the launcher; provide `LARECOMP_CC` when using a source compiler. The 0.7.3 game library was rebuilt and tested here, rather than relabeling an old DLL.

## Friends interface

### Quick emote wheel

In the overworld, click **R3 (right stick)** or press **F10** to open the wheel. Aim either stick, move the mouse, or press **1–8** to select; **A / Enter / left click** sends, and **B / Q / F10 / right click** cancels. The controller's A/B positions follow its selected Nintendo or Xbox layout. Arrow keys and the d-pad select the four cardinal options. Right-stick click has no original overworld action; its usual menu input remains available outside the overworld.

The eight choices are Hello, Cheer, Happy, Love, Surprised, Sad, Angry and Sleepy, using the supplied PNGs under `assets/emotes` (this checkout has no `assets/marks`). The wheel captures movement and interaction input while the world keeps running. Native menus, battles, loading and story scenes prevent it from opening. It closes on focus loss. A consumed controller confirm/cancel must be released before it reaches the game again.

Both trainers see the selected mark above the sender in an NPC-style white speech bubble with a short pop, decaying shake and fade after three seconds. The bubble shape is drawn by the overlay; it does not replace or redistribute the NPC's game texture. Existing head anchors, room checks and scene visibility gates apply to remote bubbles. Solo play shows the local bubble without connecting to a relay. Emote IDs 1/2 preserve Hello/Cheer; the existing protocol already supports all eight IDs. Older clients may display a generic label for IDs they do not know.

### Communicate menu and Friends panel

On Eevee 1.0.0, open the original **Communicate → Play With Others** menu for native Friends options. Its original three-button screen, cursor, button animations, touch handling and controller navigation now provide Host / Join / Online settings, or Players / Room controls / Online settings when connected. B returns one page; B at the first page returns to Communicate. These choices use our relay and bypass the Nintendo connection flow.

The native pages include player inspection, queued Hello / Cheer emotes, room-code visibility and copying, host lock / removal, confirmed departure, trainer and relay preferences, and four saved-world shortcuts with confirmed removal. Emotes send after returning to a nearby shared overworld. Text entry uses the existing PC keyboard / controller keyboard dialog. Name and relay edits apply to the next connection. Other executable builds retain the Friends overlay below.

Open **Friends** in the launcher, press **F8** in-game, or select **Friends** from the Esc / controller pause overlay. Enter a trainer name and relay address, then Host a world or Join world with its `LG-XXXXXX` code. Hosting or joining from the launcher starts the game. Neither player needs environment variables for these actions.

The page provides a masked room code with Show / Hide / Copy, player colours and activity, trainer inspection, nearby Hello / Cheer emotes, saved room codes, and Leave room / play solo. Its portrait icons are generic trainer silhouettes; the overworld uses the exchanged outfit. The host can lock new joins or remove a player. Host departure ends the room and guests continue solo; every trainer still saves independently.

Friends blocks local movement input while open but leaves the trainer visible to peers. Its own world labels are hidden behind the panel. Native menus, loading, battles and story scenes use the busy-state visibility gate. When a battle or transition removes the game's P2 actor, the bridge retries its synthetic join after returning to a stable shared overworld.

Connections retry up to three times. A dropped connection can resume its slot, colour, room ownership and room code for 30 seconds using a server-issued token; locked rooms still permit this authenticated resume. Tokens stay in memory, so application restarts do not resume a reserved session. Profile identity, relay/name preferences and the last four room codes live in `coop.ini` under `LARECOMP_SAVE_DIR`, or the data root for a normal portable launch. Saved room codes are shortcuts, not persistent hosted worlds.

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
build\LetsGoRecomp.exe --play
```

The first player's log prints a room code. On the second computer, use the same server and code:

```powershell
$env:LARECOMP_COOP_SERVER = "relay-host.example:7381"
$env:LARECOMP_COOP_ROOM = "LG-ABC234"
$env:LARECOMP_COOP_NAME = "Misty"
build\LetsGoRecomp.exe --play
```

Both players need the same title and main module build. The runtime log reports the room join and remote player join/leave. Once a peer joins, a small in-game presence indicator shows their name. R3 / F10 opens the emote wheel; both players see the selected mark in a short NPC-style bubble.

For two instances on one PC, start them with distinct save directories. Set `LARECOMP_GAMEPAD_NAME` to a case-insensitive device-name fragment from `[input] detected gamepad` to select a stable device across launches; `LARECOMP_GAMEPAD_ID` is also available for one-run diagnostics. A selected device is P1 in that instance, and the instance will not claim another pad as a local P2 controller.

Pose updates are scheduled at 60 Hz. Both endpoints disable TCP packet batching, and the client wakes for its next send deadline instead of a fixed receive timeout. Small avatar corrections use a 25–75 ms smoothing window derived from the observed snapshot cadence. The overlay shows round-trip latency to the relay, measured once per second; this is separate from game frame rate and peer-to-peer travel time.

## Current prototype limits

- The Eevee 1.0.0 actor bridge reads the native P1 trainer's world position and quaternion, plus the game's current zone hash. It sends a 32-bit fingerprint of that zone and copied pose values. Avatars are shown only for fresh peers sharing an active overworld and zone. Other executable versions are refused by the client until their actor hooks are mapped.
- Initial P2 activation waits until both clients have recent overworld poses and have exchanged appearances for at least 300 ms. The virtual P2 Joy-Con then receives a synthetic shake. The native companion spawns at the peer's world coordinates, and its transform is corrected each field update, smoothing small changes and snapping large jumps. Movement axes drive native animation; buttons and interactions are not relayed.
- The setup exchange includes the trainer's gender, style selection, and the game's 80-byte appearance resource block. The native P2 model factories use the peer's block when creating the avatar. Detected wardrobe changes automatically recreate the remote P2 model; the test verified receipt of the new style and a second factory/spawn at the shared coordinates.
- Both the local and remote trainers have nameplates at their world position plus a 150 cm trainer-height offset. They reuse the native button fill and border masks from `system_window_button_00.arc`, with preserved end caps and per-slot tint: P1 blue, P2 coral on both clients. Labels separate when trainers overlap, remain inside the window, and expire when the overworld pose stops updating. The height approximates the hat top rather than following a skeleton bone.
- Emotes are displayed as short screen-space bubbles above the nearby marker; the game does not yet play the trainer's native emote animation.
- The social actor disables its mapped static/dynamic collision components and bypasses the native distance-recovery branch. Its copied remote position is authoritative, including beyond the old 30 m cutoff. The mapped P2 party selection and battle controller-mode queries are suppressed, so a peer cannot borrow another Pokémon from the local save. Remote action buttons are never injected. These hooks still need broader coverage across story scripts and all encounter variants before claiming every game scene is audited.
- Activity packets distinguish loading, exploring, menu/loading, battle and story camera scenes. Menu versus map loading after the first world update shares a conservative busy classification. Avatars and labels hide whenever either client is busy or stale. All graphics effects may be off; nameplate camera acquisition works independently of them.
- Online sessions temporarily release a physical local P2 pad; the selected controller remains P1 in its own instance. Local co-op becomes available again after leaving online play.
- Only two peers are accepted per room. Disconnected slots are reserved for 30 seconds; no accounts or durable matchmaking are involved.
- The transport is plain TCP with session resume tokens but no TLS or account identity verification. Keep it on localhost or a trusted VPN while developing; do not expose it as a public service without adding transport security, abuse controls, and operational monitoring.

## Avatar debug run

The approved social/friends stages and proposed later features are documented in [the co-op roadmap](COOP-ROADMAP.md); shared battles and story progression remain proposals pending approval.

Local test scripts, isolated saves, logs, captures and tool caches live in the git-ignored `tests/` folder. The game executable, extracted files and runtime DLLs remain in `build/`. `tests/Start-Online-Test.ps1` starts the relay and both clients with manual controls and visible log windows.

`tests/scripts/Run-Online-Avatar-Debug.ps1` copies each online save into an isolated profile under `tests/runs/`, boots it directly, captures a renderer screenshot for each role, and exits after the capture. Start the host first, read its room code from its log, then start the joiner:

```powershell
.\tests\scripts\Run-Online-Avatar-Debug.ps1 -Role host
.\tests\scripts\Run-Online-Avatar-Debug.ps1 -Role join -RoomCode LG-XXXXXX
```

Screenshots are saved separately as `host/host-window.png` and `join/join-window.png`; logs include P2 style/state, native pose and model-factory diagnostics. `-ScreenshotFrame` changes the capture frame. `-SaveProfile complete` uses the completed workspace save. `-SaveProfile pokedex` uses English PKHeX-prepared profiles in Celadon City with 153 seen and 153 caught; the host and joiner have separate trainer names and genders. The default `starter` profile copies the original two online saves. `-EmulateControls` taps through startup prompts. Stick movement requires an explicit `-MoveRole host` or `-MoveRole join`; the default is stationary. All debug runs use isolated copies and preserve the original online saves.

## Wire protocol

Each TCP message is a little-endian 16-bit byte length followed by a one-byte opcode and body (maximum 256 bytes). Protocol **5** requires matching relay and client versions. `C` creates, `J` joins and `R` resumes. The handshake body is version (u8), title ID and main build ID (u64 LE each), code length/code for `J` or `R`, name length/UTF-8 name, profile identity (16 bytes), and resume token (16 bytes; zero for a new session).

`A` accepts with slot, code length/code, host slot, lock flag and a 16-byte token. `P` announces slot, name length/name and 16-byte identity. `H` reports host slot/lock flag. Host-only `K` sets the lock (one byte) and `D` removes another slot (one byte). `L` explicitly leaves; host leave closes the room. An unexpected drop preserves its reservation for 30 seconds. `X` rejects/ends with a length-prefixed reason. `Q`/`q` echo a 32-bit monotonic millisecond counter for RTT.

A 28-byte snapshot is area fingerprint (u32), sequence (u32), x/y/z centimetres (i32 each), yaw (u16 turns), motion/readiness (u8), left-stick axes (i16 each), and activity (u8: 0 loading, 1 exploring, 2 menu/loading, 3 battle, 4 story). Bit 7 of motion signals overworld readiness. Unready heartbeats zero the axes. `T` appearance is gender/style (u8 each) and 80 bytes of native resource IDs. `E` carries one emote byte. `U` client actions carry command (u8: 0 invite, 1 accept, 2 decline, 3 cancel/end) and invitation ID (u32 LE). Server `U` status is state (u8: 0 none, 1 pending, 2 tagged), initiator slot (u8; 255 when none), reason (u8), invitation ID and remaining milliseconds (u32 LE each). Invalid or stale actions cannot change another invitation; no save/party data is transferred. Forwarded `S`, `T` and `E` prepend the sender slot. Normal social packets carry no save blocks, party data, inventory or event flags. The opt-in `B` battle channel below exchanges an accepted guest lead and turn decisions.

## Verification on 25 October 2026

The Windows runtime was built and exercised with copied English city saves. Tests and captures are under ignored `tests/`.

| Check | Result |
| --- | --- |
| Native clients through the relay | About 180 snapshots per three seconds, roughly 16–18 ms intervals and 0–1 ms localhost relay RTT |
| Session lifecycle | Native API tests passed host, lock acknowledgement, dropped-socket authenticated resume, stable slot/code, explicit leave, new hosting, three-attempt failure and recovery |
| Relay rules | Host/guest permissions, lock, forged-token rejection, host/guest resume, kick, exact build/protocol checks, activity forwarding and reservation expiry passed |
| UI | Launcher Host and Join started sessions; guest permissions, host lock/removal, masked code controls, trainer inspection, saved-code list, guest leave, and in-game text replacement were exercised |
| Native actor | Diagnostics confirmed static/dynamic collision flags at zero; a 60 m remote offset remained authoritative with unchanged P1 coordinates and an edge marker |
| Scene gates | A peer battle state hid its actor; local battle/capture displayed no overworld labels and published activity 3 |
| Battle return | A scripted solo battle and capture returned to the city; automatic synthetic rejoin restored the remote trainer and both coloured nameplates |
| Save isolation | Original host/join save hashes remained unchanged; gameplay used isolated copies |

Testing found and fixed a launcher Host/Join ImGui stack imbalance, missing in-game modifier events, emote availability while Friends is open, the native distance-recovery loop, and remote actor recovery after battle. Coverage is of these controlled cases, not an audit of every story script or encounter variant. Internet RTT and adverse WAN jitter were not measured by the localhost tests.


## Verification after the 0.7.3 merge

- Upstream ancestry includes `60b8bdf`; merge conflicts were resolved while retaining the local classic Bag-catching mode and social hooks. The game DLL was rebuilt as `45864f025a2f58aa-v3`, in 743 seconds.
- Native Communicate Join entered a headless host room as player 2; Manage world / confirmed Leave returned to the offline Host / Join page. Captures: `tests/runs/merge-native`.
- Updated appearance packets caused a controller refresh, a second native model factory with the new style and a spawn at the peer coordinates. Captures/logs: `tests/runs/merge-avatar`.
- A copied level-50 party defeated a scripted level-3 Rattata, received native XP updates and returned to the city. The test uses upstream's new battle-relative input syntax. Logs/captures: `tests/runs/merge-battle2`.
- A framed TCP proxy injected 75?110 ms per-direction delay/jitter. Native host/guest retained about 60 Hz snapshots, with measured relay RTT 156?171 ms. This simulates network conditions; it is not a measurement of a real internet route.
- Relay invitation tests cover self-accept/stale-ID rejection, distance/area/busy gates, accept/decline/cancel/end, expiry and disconnect. Two native client workers also completed invite/accept/end.
- Scripted co-op tests use `tests/config/coop-light.ini` through `LARECOMP_SETTINGS`, keeping full graphics and frame generation out of timing checks and preserving the player's normal settings.
- Native Communicate displayed an incoming invitation, queued acceptance until returning to the overworld, and both sides acknowledged the accepted social tag. Captures and packet log: `tests/runs/teamup-final`.


## Opt-in battle extension

`LARECOMP_COOP_GUEST_CONTROL=1` enables the controlled Eevee 1.0.0 trainer trial described in [the native battle notes](COOP-BATTLE-PROTOTYPE.md#two-client-guest-move-injection-and-npc-timeout). The default social tag still leaves ordinary encounters independent. Both clients and the relay must contain this extension; capability negotiation with older relays is not implemented.

`B` payloads begin with subtype u8 and battle ID u64 LE. The relay enforces host/guest roles, explicit acceptance, current turn/sequence, legal choices and deadlines. Subtypes:

| Subtype | Body bytes | Contents after subtype and battle ID |
| --- | ---: | --- |
| 0 offer | 13 | timeout u16, trainer ID u16 |
| 1 accept | 9 | none |
| 2 lead chunk | 140 | index u8 (0/1), 130 encrypted PB7 bytes |
| 3 turn | 52 | turn/sequence u32 each, remaining ms u16, party slot u8, four move IDs u16, four PP u8, four target masks u16, two species u16, two current/max HP pairs u16 |
| 4 choice | 22 | turn/sequence u32 each, party slot u8, move index u8, move ID u16, target u8 |
| 5 outcome | 18 | turn/sequence u32 each, outcome u8 (1 guest, 2 NPC this turn) |
| 6 end | 10 | reason u8 |

A deadline defaults to 30 seconds. NPC fallback closes only the current decision; the next eligible turn returns control to the guest. Guest disconnect lets the host continue with the NPC. No guest reward, inventory or story writes are performed. This is a host-engine prototype with a guest decision overlay; full guest battle presentation remains unfinished.
