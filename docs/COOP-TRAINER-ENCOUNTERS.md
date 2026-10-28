# Co-op entry into ordinary trainer battles (roadmap priority 3)

Status: implemented, verified on copied saves (section 7) and running in normal play (section 8). `maybe_trainer()` in
`runtime/hle/coop_battle.c` stays for the factory test (`LARECOMP_COOP_TRAINER_TEST_AT`) and the guest mirror.
Eevee 1.0.0 addresses throughout.

Evidence tags:

- **[static: file]**: read from the executable or the romfs. The listings are in `tests/logs/trainer-entry/` (ignored).
- **[verified: run]**: seen at runtime. Most runtime facts come from earlier QA runs listed in
  [COOP-BATTLE-PROTOTYPE.md](COOP-BATTLE-PROTOTYPE.md).

Tools used: `tools/disasm.py`, plus the scripts in `tests/logs/trainer-entry/`:

- `natives.py`: the script native table, from the main NSO relocations.
- `amxnat.py` and `amxdis.py`: a decoder for the compact AMX64 scripts.
- `calls.py`: classifies every `CallTrainerBattleCore` call.
- `fb.py` and `fbrot.py`: trainer positions from `placement.gfpak`.

## 1. How a native trainer battle starts

### Script natives

Field scripts are AMX (Pawn) with 64-bit cells, packed in `bin/archive/script_amx.gfpak` (705 files). They bind natives by
name hash, where `h = h*0x83 ^ byte` (see `amx_Register`, `0x70006705e0`). The native table at `0x700153a710` pairs
name strings with handlers. **[static: amx-natives.txt]**

| Native | Handler | Role |
| --- | --- | --- |
| `CallTrainerBattleCore` | `0x700017e150` | builds the 0x30-byte config, calls the factory `0x700028bb60`, then suspends the script |
| `GetTrainerBattleResult_` | `0x700017e700` | reads the result from field manager (`world+0x98`) `+0xa80/+0xa84/+0xa88` |
| `CallBattleLose_` | `0x700017e880` | white-out |
| `ConvertTrainerIDIndexToHash` / `HashToIndex` | `0x700017eb90` / `0x700017ec60` | trainer index to/from its flag hash |
| `GetTrainerMsgID` | `0x700017e9d0` | params[1] = trainer index, params[2] = message type (0 = before battle, 2 = after defeat) |
| `FlagSet` / `FlagGet` | `0x70001a67b0` / `0x70001a6870` | hashed event flags |
| `IsMsgWinEnd_` / `ABKeyWait_` | `0x70001a82e0` / `0x70001a7620` | message-window polling |
| `FadeIn_` | `0x70001a7680` | script fade-in |
| `Player2Eject` | `0x7000189a80` | ejects local P2 |

The only caller of the factory `0x700028bb60` is `CallTrainerBattleCore`. Wild and static battles use `0x700028c770`
instead. **[static: disasm.py bl]**

### The generic trainer script

Ordinary overworld trainers run one shared script: gfpak entry 699, disassembled in `amx699.dis`. **[static]**

- **Sight entry** (`0x34c0`):
  1. `TempWorkGet(0)` returns the trainer hash that the field engine stored. `ConvertTrainerIDHashToIndex` turns it into
     the trainer index.
  2. The "!" balloon appears and the trainer walks to the player (`0x2250`, `0x1888`).
  3. One special trainer (unique hash `0x76a5fc2eb55e82f4`) calls `Player2Eject(0,0)`.
  4. The trainer BGM starts (`0x3168`), then the battle sequence `0x3ba0` runs.
- **Talk entry** (`0x37d0`): calls `IsBeaten(idx) = FlagGet(ConvertTrainerIDIndexToHash(idx))`.
  - If the trainer is already beaten, it shows the after-defeat text (type 2) and no battle starts.
  - Otherwise it turns the trainer, starts the BGM and runs `0x3ba0`.
- **Battle sequence `0x3ba0(idx)`**:
  1. `ShowTrainerMsg(idx, 0)` shows the pre-battle text. It is `MsgWin`, then a loop on `IsMsgWinEnd_` with `_Suspend`,
     then `ABKeyWait_` and `MsgWinClose`.
  2. `CallTrainerBattleCore(ally 0, opp1 idx, opp2 0, flags 0x200, 0, 0, 0, 0)`.
  3. `if (Lose()) CallBattleLose_()`. Here `Lose()` means result 0, or result 2 with an empty party
     (`PokePartyGetCount(1)==0`).
  4. `FadeIn_(...)` and `FadeWait_`.
  5. `FlagSet(ConvertTrainerIDIndexToHash(idx))`. This is the trainer's "already beaten" flag.
  6. The special trainer above also gets `FlagSet(0x9c8eb004e2e2b59f)`.

Neither prize money nor the defeat text runs in the script. They run inside the battle and its result process
`0x7000319bc0`. **[static]** Host money changed through that path in `mirror-save*`. **[verified: COOP-BATTLE-PROTOTYPE]**

The fade-out comes from the battle start. The fade-in comes from the script, which is why the factory-only QA had to
schedule FadeIn itself. **[static, matches the QA observation]**

### How `CallTrainerBattleCore` builds the config

The handler reads the arguments at `x1+8*n`. **[static: 0x700017e150 disassembly]**

The 0x30-byte config is `{u32 mode, u32 ally, u32 opp1, u32 opp2, u32 flags, pad, u64 p5, u64 p6, u32 p7, u32 p8}`.
The prototype doc lists the trainer fields as "ally, opp1, opp2". These are words 1-3; word 0 is the **mode**.

How the mode is chosen:

- **ally ≠ 0 or opp2 ≠ 0**: mode 1 (double or multi).
- **Otherwise**, in this order:
  1. `0x70001d8860` loads `trainer_data_%03d.bin`. Its byte 2, when nonzero, becomes the mode. Only trainers 28-31 have
     it set (story doubles).
  2. If `0x7000245750(1)` returns a P2 trainer actor, mode = 3. This is the native 2-player battle: P1 and P2 against one
     trainer.
  3. If there is no actor, controller mode (`0x7000142720`) == 2, and either `flags & 0x200` or script byte
     `ctx+0x84 → +0x39a` is set, mode = 3.
  4. Otherwise mode = 0 (single).

After the factory call:

- The process setters take `flags`:
  - `0x28dd20(0,0)`
  - `0x28dd40(1)`
  - `0x28dd70(!(f&0x80))`
  - `0x28dd80(!(f&0x100))`
  - `0x28dd50(!(f&0x20), !(f&0x40))`

  For ordinary trainers (flags `0x200`), all of these equal the constants `maybe_trainer` already passes.
- `0x7000672e50(ctx, 12)` stores wait reason 12 at `ctx+0xa4`, and the script sleeps until the battle returns.

### Who passes what

These are the arguments of every `CallTrainerBattleCore` call site in the 52 scripts that use it. **[static: calltrainer-args.txt]**

- **Script 699** (ordinary trainers) is the **only** one that passes `flags 0x200`. That flag is the game's own marker
  for "an ordinary trainer, which P2 may join".
- **Story scripts** use constant IDs and no 0x200:
  - rival `PRIVAL/ERIVAL`
  - gyms `GYM0x`
  - Rocket `ROCKET*/WROCKET*`
  - Elite Four `BIGFOUR*`
  - `BLUE`, `RED`, `GREEN`
- **Story double or tag battles** set opp2: for example script 287 is trainers 7+8 (Jessie and James), and 315, 323,
  330 and 517 are similar.
- **Script 370** uses flags `0x180`.
- **Scripts 527-534 and 562** (`TRID_MASTER_*`-style) take the trainer from a variable and pass flags 0.

Trainer names in index order are in `trainer-ids.txt`, from `bin/trainer/trainer_id_hash_table.tbl`. **[static]**

### The native tag/2-player path

The game already turns ordinary single battles into 2-vs-1 (mode 3) when a local P2 exists. That P2 battles with a
Pokémon from **P1's** party: `PARTY_SELECTION 0x70002462e0`. **[static]**

The game's double/multi path (mode 1, ally and opp2) is only used by story scripts with fixed NPC allies or opponents.
**[static]**

The prototype uses mode 1 `{1, T, T, T}`. The guest lead is imported into the battle-owned ally party
(`trainer_party_hook`, ally controller `0x7000a577f0`). That is the path with verified ownership, guest control and
replay. **[verified: ally-control8, mirror-native1, mirror-save3/4]**

The ally field can take the partner. In v1 it should keep the prototype's own construction: rewrite the config at the
factory rather than invent new script calls.

**Risk found:** `coop_actor.c` keeps the remote avatar as the native P2 actor. `social_mode_hook` only forces solo for
callers in `0x70007…-0x7000b…`, and `0x700017e3c8` is outside that range. So in today's social mode, a script trainer
battle with the avatar spawned should already select **mode 3**, and `social_party_hook` then answers P2's party
selection with slot 0. **[static, not tested.]** No existing run started a scripted trainer battle in a social session.
Verify this first (step 1 below). If it is confirmed, every non-co-op trainer battle in a session needs mode 3 forced to 0.

### Hook points (host)

| Point | Address | Sees | Use |
| --- | --- | --- | --- |
| A. pre-battle text | `GetTrainerMsgID` `0x700017e9d0`, params[2]==0 | trainer index; the battle is now certain (only reached after the beaten check) | send the B0 offer |
| B. hold | `IsMsgWinEnd_` `0x70001a82e0` | the script's own poll loop | return 0 ("not finished") while the offer is open, so the text box stays up |
| C. factory | `0x700028bb60` (chain hook), `x2` = config | final mode, ally/opp1/opp2, flags | choose co-op `{1,T,T,T}`, or solo (mode 3→0); capture the process from the `x8` handle after the original call |

Point C sees "trainer battle about to start, trainer X, single/double" in one place, for every script. **[static]**
Points A and B touch only ordinary trainers once they are gated by an active co-op offer.

## 2. Host script continuation

Host entry stays inside the real `CallTrainerBattleCore`. The script keeps its normal flow:

- the native wait (reason 12)
- `GetTrainerBattleResult_`
- loss handling: `CallBattleLose_`
- on a win: `FadeIn_`, then `FlagSet(trainer)`, so the trainer is "already beaten"

Prize money and EXP stay in the native result process `0x7000319bc0`, on the host save only.

Required changes to today's QA code:

- The QA FadeIn (`world_return_frame`, `0x700011ece0`) must run **only** for factory-started battles (QA and the guest
  mirror). A script-started battle already has its own `FadeIn_`.
- `trainer_process`, `trial_active` and `trainer_started` must be set from hook C instead of from `maybe_trainer`.

Open (static): with opp2 duplicated `{1,T,T,T}`, the host may receive twice the prize money. In `mirror-save3` the host's
money went from 47968 to 50768 (+2800), but no solo baseline exists. Compare against a `{0,0,T,0}` run with the save check before
shipping. If the prize is doubled, scale the host money delta back in hook C's after-battle path, or find a config with
one opponent party.

## 3. Eligibility, offer flow and the guest

### Eligibility

All of these must hold at hook A:

- The connection is open and the local client is the host slot.
- `CoopTeamUp.state == 2` (tagged up).
- `coop_net_remote_near(3000)`, and the guest's activity is `COOP_OVERWORLD` and fresh.
- The battle link phase is `NONE`, and no QA or mirror battle is running.
- Script trainer, single: params/config show ally 0, opp2 0, `flags & 0x200`, and trainer-data byte 2 == 0.
- The trainer is not the special `Player2Eject` trainer.

During the sight script the host's activity should stay `COOP_OVERWORLD`, because script 699 never calls
`EvCameraStart`, which is what `cutscene.c` treats as a story scene. **[static]**

Relay change: B0 must also require `team_state == 2`. Today it only checks `team_near`. Keep the relay's own
`team_near` check.

### Offer flow

1. **Hook A.** Send B0 (offer: battle id, ally timeout 30 s, trainer index). Start a local **hold window of 10 s**.
   The existing 45 s relay/client offer expiry stays as a backstop.
2. **Hook B.** While the offer is open, the pre-battle text stays on screen. Show the existing "Waiting for <partner>"
   overlay on the host.
3. **Guest.** It sees the existing prompt: Join / Skip, Enter / Escape, and the controller layout. It names the trainer
   with `coop_battle_names`.
   - **Accept** (B1): the guest sends its lead (B2 ×2). The host validates it with `coop_battle_check_import` and
     releases the hold.
   - **Decline**, the 10 s window expires, or the guest becomes busy: the host sends B6 (reason 1 or 3), releases the
     hold, and the battle is solo.
4. **Hook C.** If the lead was accepted and validated, rewrite the config to `{1,T,T,T}` and record the process for
   `coop_battle.c`. With mirror enabled, send B7/B8 as `maybe_trainer` does today. Otherwise the battle is solo:
   force mode 3→0 if step 1 confirms the risk.
5. **Battle.** Per-turn guest control is unchanged: 30 s deadline with NPC fallback, and the same B3/B4/B5 and B9-B11
   messages.

### What the guest runs

The guest does **not** need the trainer's script: no `FlagSet`, no BGM script, no `CallBattleLose_`. It keeps today's
factory replay. `maybe_trainer`'s `mirror_ready` branch calls the factory with `setup.trainer` and the same
`{1,T,T,T}`, and after the battle it uses the QA FadeIn.

The ordinary-trainer setters are the same constants, so the guest's process matches the host's. **[static]**

The guest's result phase and Spirit writes are discarded (`mirror_after_battle_hook`, `mirror_spirit_hook`).
**[verified: mirror-save3/4, mirror-abort1: money, bag, party, flags unchanged]**

The guest must start the replay only while it is on the overworld with no script running. Otherwise it aborts with B10
or B6.

## 4. Progression rules (social mode, independent saves)

- **Host:** the native result applies to the host save only: its own party's EXP, HP, PP, prize money and items, plus
  the script's trainer-defeated flag. No co-op code writes the host save.
- **Guest:** no story or trainer flags, no money, no items. Today the guest's lead and party are not changed either.
  **[verified: mirror-save*]**
- **Proposal for priority 5 (not now):**
  - Each client writes only its own save, from its own copy of the battle.
  - The guest applies its lead's EXP/level/PP/HP deltas only after a **finished** replay: a matching end count and no
    desync. The deltas are keyed by battle id, so a resume or replay cannot apply them twice.
  - Money: a flat "each player receives the trainer's normal single prize once" rule, computed locally from the trainer
    data. It is never forwarded over the network.
  - The trainer-defeated flag stays host-only. A guest can still battle that trainer in its own world later.
  - Gyms, badges, rival, Rocket and league stay individual and are excluded anyway.

## 5. Edge cases and v1 exclusions

### Excluded in v1

Everything that does not pass through script 699 with flags 0x200, single:

- gym leaders, rival, Rocket, Elite Four and Champion, `RED/GREEN/BLUE`
- Master and Coach trainers (scripts 527-534, 562)
- story doubles: trainer-data byte 2, or opp2 set
- multi/tag battles with a native ally
- the special `Player2Eject` trainer
- wild and static battles (they use separate code)

### Edge cases

| Case | Behaviour |
| --- | --- |
| Trainer spots the host while the guest is far, in a menu, in a battle or loading | Not eligible at hook A. No offer, solo battle (with mode 3 forced to 0 if needed). |
| Guest opens a menu or starts its own battle during the offer | The guest declines automatically (B6 reason 1). The host releases the hold and goes solo. |
| Host talks to an already-beaten trainer | Hook A is never reached (type 2 message only). No offer. |
| Two opponents / native double | Excluded by the gate (mode ≠ 0 before the rewrite, or opp2 ≠ 0). |
| Disconnect while the offer is open | The link phase ends. Hook B releases at once and hook C sees no accepted lead, so the battle is solo. The relay clears the battle (reason 2). |
| Disconnect after hook C | Existing behaviour: the NPC finishes the ally's turns, and the guest replay aborts with its results discarded. |
| Host loses | The script's `CallBattleLose_` whites out the host only. The guest has no script and its results are discarded. |
| Host gets a second trainer before returning | Allow a new offer only once the previous battle id has ended (link phase `NONE`). |
| Accepted lead fails validation | B6 reason 1, solo battle. |

## 6. Implementation plan (one agent, after the coop_battle.c work lands)

1. **Verify the mode-3 risk.**
   - Run one social pair: host plus a synthetic or real guest with the avatar spawned. Move the host in front of an
     unbeaten ordinary trainer with `tests/scripts/run-trainer-entry.sh` (`MAP=<area hash> <run> <x> <y> <yaw>`).
     Positions come from `tests/logs/trainer-entry/placement-trainers.txt`. Map ids are `fnv1a64(area name)`, see
     `tests/logs/area-map-ids.txt`.
   - Log the config at `0x700028bb60` and the natives listed above.
   - My two solo attempts did not get a battle. Forest `TRID_INSECT_01` at (3885, 1550) and Route 13 `TRID_BOY_15` did
     not engage with the fixture save, which is probably already beaten or out of sight; see `tests/runs/trainer-entry2`.
     `LARECOMP_HOOK_DUMP` printed nothing, so it is unconfirmed that dump hooks fire for natives. Prefer a small logging
     hook in a new file.
2. **New file `runtime/hle/coop_trainer_entry.c`.**
   - Chain hooks A, B and C (`rt_register_fnhook_chain`), installed from `coop_battle_install_hooks`'s caller.
   - Keep a small state machine: idle → offered(id, trainer, deadline) → accepted/lead-ok | solo → started(process).
3. **Small `coop_battle.c` API.**
   - `coop_battle_link_ready_for(trainer)`: the lead check and B7/B8 send, factored out of `maybe_trainer`.
   - `coop_battle_script_started(process, trainer)`: sets `trainer_process`, `trial_active`, `trainer_started`, and
     disables the QA FadeIn.
   - Keep `maybe_trainer` for QA and for the guest mirror only.
4. **Relay.** B0 requires `team_state==2`. Add tests in `tests/tools/battle-mirror-relay-test.py`: B0 rejected without a
   tag; offer expiry; decline.
5. **Guest UI.** Reuse the existing offer prompt, showing the trainer name and partner name. Auto-skip it when the guest
   is busy.
6. **Tests on copied saves.** Run each of these with the save check (`run-mirror-save.ps1 -SaveCheck`):
   - accept: the host trainer flag is set, the host is FadeIn'd with no QA fade, and the guest save is unchanged
   - decline: a normal solo battle
   - hold expiry
   - guest busy
   - disconnect during the offer and after the first turn
   - host loss
   - an already-beaten trainer
   - a gym trainer as a negative case
   - a prize-money comparison against a solo baseline

## Evidence files

- `tests/logs/trainer-entry/amx-natives.txt`: native name, handler and table slot.
- `tests/logs/trainer-entry/amx699.dis`: the generic trainer script.
- `tests/logs/trainer-entry/dis/*.dis`: all 52 battle scripts.
- `tests/logs/trainer-entry/calltrainer-args.txt`: per-script `CallTrainerBattleCore` arguments.
- `tests/logs/trainer-entry/trainer-ids.txt`: index, flag hash and `TRID_*` name.
- `tests/logs/trainer-entry/placement-trainers.txt`: trainer positions per placement file. File 11 is Viridian Forest
  and file 7 is area08. These are heuristic flatbuffer reads.
- `tests/scripts/run-trainer-entry.sh`: a solo copied-save run with a moved player and hook dumps.
- `tests/runs/trainer-entry2`, `trainer-entry3`: the attempts above. They contain no trainer battle.

## 7. Implementation and verification (26 October 2026)

Code:

- `runtime/hle/coop_actor.c`: `social_trainer_hook` (trainer getter `0x7000245750`, caller `0x700017e3bc`) and
  `social_mode_hook` (caller `0x700017e3cc`) keep every online trainer battle without an accepted team-up solo. This
  applies to all social sessions.
- `runtime/hle/coop_battle.c`: hooks A-C as designed (`trainer_msg_hook`, `msg_end_hook`, `trainer_core_hook`) plus
  `process_setter_hook` (`0x700028dd20`, the first setter after the factory) to capture the process. The team-up is
  made by setting ally and opponent 2 in the native's own arguments, so the native picks mode 1 `{1,T,T,T}`.
  The QA FadeIn now runs only for factory-started battles. Like the rest of the battle prototype, the hooks are
  installed only with `LARECOMP_COOP_PARTY_TEST_DIR` and act only with `LARECOMP_COOP_GUEST_CONTROL=1`.
- The hold: the offer's link deadline is shortened to 10 s (`coop_battle_link_offer_deadline`), so the link itself
  expires it (B6 reason 3) and the host overlay counts down from 10.
- Prize: `{1,T,T,T}` has two copies of the trainer and the result added twice the prize (+72 instead of +36). The
  host's prize add (`0x70001c6f60` called from `0x7000319e0c`) was halved for script team-ups; since 28 October the
  trainer prize itself (`0x70009d08d0`) is halved, so Pay Day stays whole (section 10).
- Guest items: the trainer's 3 Poke Balls are added during the battle (bag add `0x70001c8e20`), not in the discarded
  result phase. The mirrored guest's bag adds are discarded.
- Relay: B0 requires `team_state == 2`. `LARECOMP_COOP_AUTO_TAG=1` (QA) tags up automatically; the pair scripts set it.

Verification (`tests/scripts/run-trainer-coop.sh`, Viridian Forest Bug Catcher `TRID_INSECT_01` = trainer 54, beaten
flag = event flag 2742; host = `coop-test-save` copy with flag 2742 cleared, `tests/saves/trainer-coop/`):

| Run | Result |
| --- | --- |
| `trainer-coop-base-social1` (unmodified build, social, no tag) | **Risk confirmed**: native 2-vs-1, host Eevee + Pikachu from the host's own party, and the battle waits forever for P2's menu. |
| `trainer-coop-base-solo1` (unmodified build, offline) | Baseline: money +36, 3 Poke Balls, flag 2742 set, Bite PP 25 -> 24. |
| `trainer-coop-social2` | Social, no tag: no offer, 1-vs-1, same save changes as the baseline. |
| `trainer-coop-timeout2` | Tagged, guest silent: text held ~10 s ("Waiting for ally to join"), then solo, same as the baseline. |
| `trainer-coop-accept2` | Team-up: host Eevee + guest Machamp vs two Kakuna, guest Bite injected, matching digest, both back on the overworld. Host: money +36, 3 Poke Balls, flag 2742, same as the baseline. Guest save pre/post PASS (orig/post: only the known load noise, work 675 and MyStatus). |
| `trainer-coop-qa1` | Regression: the factory QA (`run-mirror-save.ps1 -SaveCheck`) with the tag requirement; guest PASS. |

Not done (see section 8 for what normal play covers now): a guest busy/disconnect during the hold, host loss, a gym trainer as a negative case, naming the trainer in
the guest prompt and auto-declining a busy guest. In automated runs the native P2 avatar only joins on a save past the
early game (the `play-pewter` save never joined it).

## 8. Normal play (27 October 2026)

The team-up runs without QA variables (user-facing description: [COOP-ONLINE.md](COOP-ONLINE.md#team-up-trainer-battles)):

- `coop_battle_install_hooks` installs every hook on Eevee 1.0.0; each passes through unless a team-up (or QA) battle runs.
  `coop_battle_link_enabled()` and `coop_battle_link_mirror_enabled()` are on unless set to 0. Files (journals, party
  files, save dumps) are written only with `LARECOMP_COOP_PARTY_TEST_DIR`; the factory trigger, fixed seeds, fault
  injection and auto accept/move/tag keep their variables. The guest replay (`maybe_trainer`'s mirror branch) no
  longer needs the QA directories and starts only while the guest is on the overworld.
- Setting "Team-up battles" (`coop.ini teamup_battles`, default 1): native Online settings → More settings, or the
  Friends (F8) checkbox. Off: `coop_battle_link_offer` refuses and a received B0 is answered with B6 reason 1.
- A guest that is not on the overworld for 0.5 s while an offer is open declines it (B6 reason 1,
  `coop_battle_link_guest_ready`, fed by the network worker from the local activity).
- The guest prompt names the host and the trainer (`trname.dat`: trainer 54 is "Doug").
- Protocol 6 (client and relay): protocol-5 builds are refused when connecting.

Verification: `tests/scripts/run-teamup-play.sh` (two clients, copied saves, the tag-up through the native Team up
menu, input only through `LARECOMP_HOST_INPUT_FILE`), runs in `tests/runs/teamup-play1/`.

| Run (`tests/runs/teamup-play1/`) | Result |
| --- | --- |
| `accept` | Tag-up through Communicate → Play With Others → Players → P2 → Team up (both clients). Offer "Host was challenged by Doug!", Join, host Eevee + guest Machamp vs two Kakuna, guest chose Bite in its native menu (`g_moves`, `g_target`, `g_wait`), both back on the overworld, prize 72 → 36. |
| `accept3` | Tag-up through Friends (F8). Guest saved right before the challenge (`save-pre.bin`); the guest did not answer turn 1, the NPC chose and the battle ended. Guest pre/post **PASS**; orig/post only the known load noise (MyStatus, work 675, block 15). Host orig/post equals the solo run `off`: money +36, item 4 (Poke Ball) 50 → 53, flag 2742, Bite PP 25 → 24. |
| `off` | Both settings off: "no team-up offer (Team-up battles off)", 1-vs-1, host rewards as above, guest unchanged. |
| `guestoff` | Guest setting off: offer declined at once (B6 reason 1, host log `entry_ended` within the same second), solo battle, same host rewards. |
| `guestinbattle` | Guest in a wild battle when the trainer spoke: no offer (`near 0`), solo. |
| `late-answer` | Guest answered after the 10 s hold: the offer expired, solo. |

Only the variables listed in each `env.txt` were set (data root, save copy, per-run ini, the documented `LARECOMP_COOP_SERVER/HOST/ROOM`, `LARECOMP_NO_USER_INPUT`, `LARECOMP_HOST_INPUT_FILE`, capture dir, `LARECOMP_WINDOW_SHOT` + `LARECOMP_EXIT_AFTER_SHOT`); no co-op QA variable. The games wrote no files besides `coop.ini` and the saves.

Still open: the busy decline while an offer is already open (unit-tested in `battle-link-test.c` only), disconnects during the hold, host loss, a gym trainer as a negative case, and battles started by the room's guest (never offered).

## 9. Switching, items and fainting (27 October 2026)

Details, addresses and runs are in [COOP-BATTLE-PROTOTYPE.md](COOP-BATTLE-PROTOTYPE.md#switches-replacements-and-items-27-october-2026).
In short:

- **Parties.** On accepting, the guest sends its whole party (B2, up to six Pokémon), not just its lead. The ally
  trainer's battle party is that party on both clients, so the ally sends in only the guest's own Pokémon. The
  trainer's own Pokémon are no longer used as the ally's reserves. The opposing side is still `{1,T,T,T}`.
- **Ownership.** Each player decides only for their own slot: the host for slot 0 in its own menus, the guest for
  slot 2 through B4. This covers the action menu (move or *Pokémon* switch) and the switch-in after a faint
  (request 4) or mid-turn (request 5). Every decision reaches the guest's replay as the native command word (B9).
  The guest's *Pokémon* list and its switch-in list show the guest's own party.
- **Deadline.** A switch-in has the same 30 s deadline; after it, the NPC picks that one replacement. A player whose
  last Pokémon faints is out, and the native multi battle goes on with the other player.
- **Items.** The host's Bag works and spends from the host's bag only; the guest's replay skips the bag removal.
  The guest's Bag is refused natively and the action menu comes back. Decided because the battle spends items from
  the local slot's bag only, so a guest item would come from no one's bag.
- **Progression** (section 4) is unchanged. The guest's Pokémon fight as copies: fainting, HP, PP and items used in
  the team-up never reach the guest's save. Verified with `savediff` in `tests/runs/teamup-switch2/*`.
- **Protocol 8** (client and relay; 8 changed the turn digest, see COOP-BATTLE-PROTOTYPE.md); older builds are refused when connecting.

## 10. Partner rewards (28 October 2026)

Details, addresses and runs: [COOP-BATTLE-PROTOTYPE.md](COOP-BATTLE-PROTOTYPE.md#partner-rewards-pay-day-trapping-and-mid-turn-switches-28-october-2026).
User-facing rules: [COOP-ONLINE.md](COOP-ONLINE.md#team-up-trainer-battles). Protocol 9.

- **What the partner gets** for a won team-up, in its own save only: EXP for its own Pokemon and one trainer's prize
  (the same amount the host gets). The EXP is worked out by the game's own calculation, run on the partner's replay
  of the battle at each knock-out: participation, the shared part, Let's Go's boosts. Level-ups use the game's
  out-of-battle level-up, and evolutions play right after the battle.
- **What it never gets:** the trainer's items, Pay Day money, the beaten flag, badges or story flags. Its replay runs
  no trainer script, so social mode stays independent: the partner can still battle that trainer in their own world.
- **When it gets nothing:** an aborted replay (desync), a lost link before the host's finished end, a lost battle,
  or a final state that does not match the host's. A battle ID is rewarded once, at the result phase of a replay that
  ran, so a replayed message or a reconnect cannot add a second reward. A crash before the next in-game save loses
  the rewards, as it would lose a solo battle's.
- **Host:** unchanged, apart from Pay Day now staying whole: one trainer's prize, the trainer's items, the beaten flag.
- Gyms, badges, rival and league battles stay individual: only script 699's ordinary single trainers (flags `0x200`,
  Master trainers excluded) become team-ups (section 7).
- Verified on copied saves with `savediff`: `teamup-rewards2/payday` (no QA variables), and also `teamup-rewards2/abort`,
  `/loss`, `/disconnect`, `teamup-midturn2/*` and the solo baseline `teamup-rewards1/solo`.


## 11. Wild team-ups (28 October 2026)

User-facing rules: [COOP-ONLINE.md](COOP-ONLINE.md#team-up-wild-battles). Engine details and addresses:
[COOP-BATTLE-PROTOTYPE.md](COOP-BATTLE-PROTOTYPE.md#wild-team-ups-and-the-partners-move-targets-28-october-2026).
Protocol 10.

### Scope and decisions

- **Classic wild battles** (the PC setting "Wild battles", `wildbattle.c`'s synthetic `WILD_HASH` encounters with the
  battle Bag) become team-ups: the host and the partner against the one wild Pokemon, in Let's Go's own 2 vs 1.
- **Let's Go catch mode** (the default, the ball-throw minigame) stays solo. It is not a battle: there is no turn
  engine to replay and no command stream, and the throws, motion and berry state live in the capture scene. A shared
  catch would need a second synchronised scene and a new rule for who keeps the Pokemon, so it is left as it is.
  A partner who has joined a team-up is not touched by wild Pokemon until its replay ends (either mode).
- **Story statics** (`CallWildBattle`: Snorlax, the birds, Mewtwo, Electrode...) stay solo. Their battle runs the
  legendary rule (a knock-out ends it, no EXP), then the GO capture, then the script's flags that make the static
  disappear. A partner would get nothing (no EXP, no catch) and its replay would have to run that script's battle
  without the flags. `coop_actor.c` keeps them solo (`social_trainer_hook` / `social_mode_hook`, return addresses
  `0x700017e4c0` / `0x700017e4d0`), unchanged; only `WILD_HASH` battles are offered.
- **Catch ownership: the catch belongs to the ball's thrower, and only the host can throw.** The partner's Bag is
  refused in its menu (as in trainer team-ups); the host's Bag works and spends the host's balls. On the partner's
  replay a caught battle (result 5) is turned into a run (result 3) before the result phase, so
  `wildbattle_classic.c`'s `store_caught` never puts it in the partner's save, and the replay's game records (total
  captured...) are skipped (`coop_actor.c` `replay_record_hook` on the record increment `0x70001cac40`). A guest-owned throw from its own bag was not
  attempted: the battle spends items from the local slot's bag only, so the host's engine would throw a ball from no
  one's bag and the replay would have to store a Pokemon in the other save.
- **EXP:** as in trainer team-ups, from the partner's own replay after the end digest matches, once per battle ID
  (`partner_exp`, `partner_rewards`), only for a won battle (the wild Pokemon knocked out). No prize money (wild).
- **Fleeing:** only the host can run (the partner's Run reopens its menu); the escape ends the battle for both, and
  the partner gets nothing.
- **Pokedex:** the partner's replay registers the wild Pokemon as seen, as any battle would. Nothing else is written.

### How a wild team-up runs

1. **Offer at the touch.** `wildbattle.c` `trigger_hook` calls `coop_battle_wild_touch(species)` while both players
   are still on the overworld (the relay's `team_near` needs both there). The B0 trainer field is
   `0x8000 | species`; the guest's prompt reads "*Host* found a wild *species*! ...".
2. **Hold.** The battle event `0x700028be80` (kind 2, static) is held at state 0, before its setup, for up to 10 s
   (`wild_event_hook`, `entry_poll`); the screen has faded out and the host's overlay counts down. Decline, expiry, a
   busy partner or a lost link: the setup runs untouched, solo.
3. **Setup.** `wild_setup_hook` (`0x70009ccda0`, the static/wild setup builder) adds an ally trainer the way the multi
   builder `0x70009cd7e0` does: party `+0xc0` with the guest's whole party, trainer data `+0x448` and info `+0x270` of
   trainer 54 (its NPC chooses at a deadline; the model is the partner's avatar), mode 3. The host sends its party
   (B7), the wild Pokemon as its battle built it (B7 chunks 12-13: PID, nature, IVs differ per game) and the seed with
   its identity (B8, below).
4. **The 2 vs 1 with an ally.** Rule 3's second player position is natively a second Pokemon of trainer 0 (local P2
   borrows one of the player's own). While a wild team-up runs, the rule's position table, the positions per trainer
   and the NPC controller table give position 2 to trainer 2 (`rule3_ally_*`), and the main module init of non-multi
   battles, which builds only trainers 0 and 1, builds trainer 2's parties too (`main_init_hook`). A multi battle
   (rule 1) instead needs an opponent 2: its empty party crashes the battle party setup (`teamup-wild1/e1`).
5. **The partner's replay** starts with `wildbattle_replay()` (the same synthetic battle as a touch) once it is on the
   overworld; its setup takes the host's wild Pokemon and its own party as the ally. From there the trainer
   team-up's machinery is unchanged: per-round digests, the partner's native menu for slot 2 (the 2 vs 1 reads that
   menu from P2's controller, a sideways right Joy-Con, so `input.c` gives P2 this player's controls while it is open,
   with the d-pad as its stick), NPC at the deadline,
   switches, rewards.

### Replay fixes found by the wild team-ups (trainer team-ups too)

- **EXP of a host Pokemon below level 100.** The EXP formula's "own Pokemon" (`0x70001c9d90`) and player language
  (`0x70001c9d40`) answer for the local player; on the partner's replay the host's Pokemon counted as traded (x1.5),
  so it reached another level and the digest broke. The host now sends which of its party slots are its own and its
  language with the seed (B8, 19 bytes), and the replay's EXP step answers as the host did.
- **Level-ups on the replay.** The battle builds trainer 0's party twice, for the server's and the client's pool,
  and applies a level-up to each. The replay used one clone of the host's party for both, so the second level-up
  started from the first's stats (HP 3 above the host's, `teamup-wild2/host-out4`, traced with `LARECOMP_WATCH`).
  Each pool now gets its own clone.
- All earlier trainer runs used level-100 hosts, which gain no EXP, so neither showed there.

### Verification (two clients, copied saves, `tests/scripts/run-teamup-wild.sh pair`, `run-teamup-wild-suite.sh`)

Runs in `tests/runs/teamup-wild1/` (engine bring-up) and `tests/runs/teamup-wild2/` (two clients). The savediff
lists the reward blocks (party, money, Pokedex, records) as `[CHANGED]` against a trainer-oriented allowed list; each
change was checked to be the battle's EXP, evolution or seen entry.

| Run | What | Result |
| --- | --- | --- |
| `teamup-wild1/e1` | rule 1 (multi) with an empty opponent 2 | crash in the party setup: rule 3 chosen instead |
| `teamup-wild1/e12`, `base1`-`base4` | first rule-3 2 vs 1 with the QA ally (one game) | battle runs to the end |
| `teamup-wild2/basic2` | host + partner vs Snorlax, partner chooses in its native menu | digests match, partner EXP once, Charmander evolves, host Pay Day +500 |
| `teamup-wild2/targets5` | partner picks Swords Dance, Growl, Earthquake by d-pad | Growl (all foes) and Earthquake (all adjacent) accepted; Swords Dance (self) was refused, fixed after the run (any target for moves that name no one), not re-run |
| `teamup-wild2/flee2` | host runs | both result 3, no rewards |
| `teamup-wild2/catch2`, `catch3` | host throws a Master Ball | caught Pokemon stored on the host only; partner result 3, no records, no Pokemon |
| `teamup-wild2/decline` | partner declines | host battles solo |
| `teamup-wild2/guestwild-offer` | a wild Pokemon touches the partner during the offer | the partner's own battle declines the offer; host solo |
| `teamup-wild2/guestwild-accepted` | ... right after it joined | the touch is ignored; the replay is fine |
| `teamup-wild2/guestwild-first` | the idle partner is in its own battle (an evolution) at the host's touch | no offer (busy), host solo |
| `teamup-wild2/guest-idle2`, `guest-idle3` | partner never chooses | its NPC chooses at the 10 s deadline; digests match, rewards applied |
| `teamup-wild2/host-out6`, `host-levels` | level-2 host Eevee levels to 8 on both sides | digests match after the EXP and level-up fixes |
| `teamup-wild2/trainer-regress`, `trainer-host20` | trainer team-up (Doug) after the changes | same as the Pay Day baseline; level-20 host digests match |

Unit and relay checks: `tests/scripts/run-battle-link-tests.sh coop-link-wild2` (wild B0/B7/B8 on the link and the
relay, protocol 10), all pass.

### Still open

- **Move learning on the host's level-ups.** The replay answers "forget a move?" by itself (it presses A: yes, the
  first move). If the host chooses differently, the next digest differs, the replay aborts and the partner gets no
  rewards for that battle; neither save is harmed. Syncing the host's choice needs its answer to travel like a
  decision; not done.
- The ally's trainer data is trainer 54's; its trainer name only appears in texts that name the ally trainer.
- Not run after the last changes: a partner's self-target and side moves (Swords Dance, Light Screen) in the native
  menu, and its d-pad on a real controller; a team-up after the partner's own wild battle (`guestwild-first` with a
  partner that does not evolve).
