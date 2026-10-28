# Online co-op proposal

Status: stages 1–2 implemented and tested in controlled scenarios, with independent progression. Further stages were requested on 25 October 2026. Stage 3 has voluntary social tag-up invitations and, since 27 October 2026 (protocol 6), team-up battles against ordinary trainers in normal play (setting "Team-up battles", default on; [COOP-ONLINE.md](COOP-ONLINE.md#team-up-trainer-battles)). Since protocol 7 each partner switches in their own Pokémon and the host uses its Bag. Since protocol 9 (28 October 2026) the partner gets its own rewards for a won team-up, in its own save: EXP and one trainer's prize, with level-ups and evolutions. It never gets items, flags or badges. Since protocol 10 (28 October 2026) the host's wild battles in the classic "Wild battles" mode are team-ups too: a Pokémon caught with the host's ball is the host's, the partner gets EXP for a knock-out, and Let's Go's catch mode and story statics (Snorlax, the legendaries) stay solo ([COOP-TRAINER-ENCOUNTERS.md section 11](COOP-TRAINER-ENCOUNTERS.md#11-wild-team-ups-28-october-2026)). The partner can use every move in its menu. The partner's own Bag and the host's move-learning choices on the partner's replay are still open. Shared story scenes (4), trades, practice PvP and spectating (5) remain unimplemented. Details and limits are in [COOP-ONLINE.md](COOP-ONLINE.md).

## Reference experience

Platinum Together provides a useful interaction model:

- [Playing with friends](https://platinumtogether.online/wiki/#/guide/friends): relay room codes, a player list, host controls, reconnection and separate saves.
- [Team-ups](https://platinumtogether.online/wiki/#/guide/team-ups): explicit nearby tag-up invitations, shared battles, individual boss progression, trading and spectating.
- [Story scenes](https://platinumtogether.online/wiki/#/guide/story-scenes): readiness prompts, host-led story progress, catch-up and one-time rewards.

These are references for our design, not features already supported by this project. Let’s Go has different battle and event systems, so each needs its own implementation.

## Proposed scope and defaults

Keep **Social** as the default: each player owns their save, party, inventory, encounters and story progress. Players can see one another and interact voluntarily. Entering a room must not advance another player's story or award items.

Offer **Shared Adventure** later as an explicit opt-in mode. Each player still has their own save file, but selected story events and ordinary trainer victories can affect both saves. This changes the original independent-progression requirement and needs approval before implementation.

Start with two players. Four-player rooms require additional independent actor replicas; increasing the relay capacity alone will not work. Keep the existing controller-per-client setup.

## 1. Make the social foundation reliable

Turn the native local-co-op actor into a controlled remote representation. Audit and disable its local co-op gameplay effects: camera leash, automatic battle participation, item collection and event triggers. Preserve visual movement and appearance.

Exchange explicit states for overworld, loading, menus, battles and story scenes. Show avatars and head labels only when both clients can share the same scene. Show a useful busy status in the player list otherwise. Preserve stable player identity and colour through reconnects.

Validate protocol/game compatibility before spawning an avatar. Reconnect automatically with bounded retries and clear failure messages. Measure relay RTT, snapshot age and render frame time separately; tune interpolation from actual network measurements. Two local instances also need a lightweight test graphics profile so GPU stalls are not confused with network delay.

Acceptance: both clients move independently; change rooms, open menus, enter battles, disconnect and rejoin without phantom avatars, input theft or unintended save changes. Manual sessions remain open and receive no scripted movement.

## 2. Add the friends interface

Provide Host / Join from the launcher and pause UI, a masked room code with Show / Copy, connection status, and a player list with names, portraits, colours and activity. Add host lock, remove player, leave room, saved room codes and reconnection controls. No account or Discord dependency is proposed.

Add nearby social actions: inspect a trainer and simple emotes. A later “meet up” action must respect the receiving player's unlocked travel destinations and must not teleport into locked maps or active scenes.

Acceptance: two players can start, find each other, reconnect and leave through the UI without environment variables or terminal commands. Leaving keeps both saves independent.

## 3. Add voluntary battle team-ups

Add Tag Up / End Tag invitations with accept, decline, cancellation, proximity and busy checks. Joining a world does not automatically join battles.

Implemented foundation: relay-authoritative invitation IDs, accept/decline/cancel/end, 45-second expiry, current-area/distance/activity checks, disconnect cancellation, Friends controls and native Communicate pages. A current tag is social travel only: it does not enable native P2 battle mode or import the host's party. Native client and relay lifecycle tests pass. The native prototype now exports/imports pointer-free own-party snapshots, verifies temporary Pok?mon round-trips without changing the local party, and enters an ordinary trainer battle on copied saves. A foreign lead has also rendered in the temporary ally slot; the initial experiment used native NPC decisions. See [COOP-BATTLE-PROTOTYPE.md](COOP-BATTLE-PROTOTYPE.md). Ordinary move/target commits and cancellation are now observed and tested, with an isolated authority gate for player ownership, ordered turns and replay/disconnect rejection. The isolated two-human-command gate is not wired into native execution. A separate opt-in two-client ally deadline path now exchanges the guest lead after consent, injects validated moves/targets into the host's native ally slot, shows waiting/countdown UI, and uses the native NPC for a missed turn before restoring guest control next turn. Controller/keyboard, target selection, timeout recovery, disconnect continuation and copied-save checks have run automatically. Guest native battle presentation/state replication, ordinary trainer integration and reward ownership remain open.

First investigate the native battle hooks and prototype one ordinary trainer battle. Each client must command its own Pokémon from its own party. Design a single battle authority, ordered commands, state validation and disconnect handling before generalizing to wild battles. Simply forwarding local P2 inputs would use the host's party and does not meet this requirement.

Define EXP, catch ownership, inventory changes and reward ownership explicitly. Keep gyms, badges, rival and league victories individual. Ordinary trainer completion may be shared only under the approved Shared Adventure policy.

Acceptance: a controlled battle produces matching battle state while persisting only each player's own agreed rewards. Declining a tag request leaves normal solo play intact. Disconnects cannot duplicate rewards or corrupt either save.

## 4. Add optional shared story scenes

Implement this only after agreement on Shared Adventure. Start with one mapped, reversible scene rather than arbitrary event flags or whole-save copying.

Use a ready prompt: Ready, Not Yet, Stay Out, initiator Cancel and a clearly offered Continue Without. Offer Everyone or Nearby participation and Ask by default; automatic nearby joining is optional. Include a 30-second reconnect grace period where appropriate and a choice to stay together or return after the scene.

Maintain an event ledger for selected completion flags and one-time gifts. Catch-up must be explicit and idempotent: reconnecting or replaying messages cannot duplicate the Pokédex, items or rewards. Badges and major individual victories remain personal. Social mode never applies this ledger to another player's progress.

Acceptance: test different initial story progress, refusal, cancellation, disconnect at each stage and repeated catch-up. Confirm exactly-once rewards and no progress changes in Social mode using copied saves.

## 5. Add trades, practice battles and spectating

Treat these as separate features after battle synchronization is reliable. Trades need inspectable offers, confirmation from both players, both confirmations cleared by any offer change, durable transaction IDs and recovery after interruption. Save both sides consistently; never exchange entire save files.

Practice PvP must not persist temporary battle damage or consume normal inventory. Spectators receive read-only state, can stop watching and cannot issue battle commands.

Acceptance: offer changes, disconnects and restarts cannot duplicate or lose Pokémon; practice battles preserve pre-battle party state; spectators cannot affect the result.

## Delivery and approval

Initial approval covered stages 1 and 2. Subsequent instructions authorize continuing the battle prototype, guest decisions with per-turn NPC timeout recovery, remaining roadmap work and automated self-testing. Social remains the default. Approve Shared Adventure separately before any cross-save progression work. Four-player support and account-based invitations remain later extensions.

Keep all copied saves, capture scripts, logs and test executables under ignored `tests/`. Build and network checks run for each relevant change; screenshots verify both clients' appearance and labels. Save-affecting features require copied-save comparisons and interrupted-session tests before normal play.
