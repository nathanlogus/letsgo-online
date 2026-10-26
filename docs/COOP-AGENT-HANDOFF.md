# Co-op development handoff

Prepared 26 October 2026. Give this document to a new agent/session to continue the work. Process IDs, working-tree state and available binaries must be checked again when resuming.

## Instructions for the next session

Continue development in `C:\Users\Nathan\orca\letsgo-recomp`.

First read the repository instructions, inspect git status, and read:

- `docs/COOP-ROADMAP.md`
- `docs/COOP-BATTLE-PROTOTYPE.md`
- `docs/COOP-ONLINE.md`

Some roadmap status paragraphs predate the latest prototype results; reconcile them with the appended test evidence and actual code.

Goal: finish the remaining social co-op and battle work autonomously, with automated testing. Do not require the user to manually test.

## Current state

- Social overworld co-op, remote avatars, appearance, nameplates, emote wheel, Friends UI and native Communicate integration are implemented.
- Voluntary tag-up invitations and guest move/target selection work.
- A missed guest choice uses NPC control for that turn; guest control returns next turn. Do not end the battle on timeout.
- Experimental guest native battle replay works in one controlled two-turn encounter.
- `tests/runs/mirror-native1` contains host/guest journals, captures and `verification.json`. Both clients returned to the overworld; their two recorded turn snapshots matched, and the guest's local party was byte-identical before/after.
- These snapshots are partial diagnostics, not proof of complete battle determinism.
- Experimental replay requires `LARECOMP_COOP_GUEST_CONTROL=1` and `LARECOMP_COOP_BATTLE_MIRROR=1` and remains isolated to copied-save QA.
- The battle prototype's mapped native addresses currently target Eevee 1.0.0. Do not assume other versions are supported.

## Relevant code

- `runtime/hle/coop_battle.c`
- `runtime/coop_battle_command.c/.h`
- `runtime/coop_battle_link.c/.h`
- `runtime/coop_battle_names.c/.h`
- `runtime/coop_party.c`
- `runtime/coop_net.c`
- `runtime/ui/coop_ui.mm`
- `runtime/ui/menu.mm`
- `server/coop_relay.py`

## Priorities

1. Harden replay synchronization and save isolation. Test longer encounters, varied moves/status effects, latency, disconnects, stale messages and end-of-battle ordering. Expand state comparison and add safe mismatch handling.
2. Verify all guest save effects, including money, inventory, EXP, story flags and partner effects, not only party bytes. The mirrored native result screen currently displays normal reward text; do not assume those rewards are safely isolated.
3. Integrate voluntary team-ups with actual trainer encounters and normal UI, replacing test-only battle entry.
4. Add switching, items, fainting, forced replacement and special targeting with validated ownership.
5. Implement explicitly defined per-player HP/PP/EXP/reward persistence without duplication.
6. Polish guest perspective, participant names, waiting messages and result presentation.
7. Continue remaining roadmap features such as trades, practice PvP and spectating after battle reliability is established.

## Constraints

- Social mode and independent progression remain the default.
- Do not copy whole saves or silently synchronize story flags, badges, gyms or league progress.
- Shared Adventure progression requires a separate explicit agreement.
- Preserve unrelated and concurrent working-tree changes. At handoff there are day/night and rendering/settings changes that are not part of the battle work.
- Do not overwrite or stop a user's running game. Inspect processes first.
- Use copied saves only. Keep test scripts, logs, captures, fixtures and binaries under the git-ignored `tests/` directory.
- Preserve the original manual saves under `tests/saves/coop-manual-synced`.
- No commit or push unless requested.

## Build

Use an isolated runtime build from PowerShell:

```powershell
& C:/msys64/usr/bin/bash.exe -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /c/Users/Nathan/orca/letsgo-recomp && make -j8 B=tests/build-coop-mirror tests/build-coop-mirror/LetsGoRecomp.exe'
```

The executable must retain the filename `LetsGoRecomp.exe` because the extracted game DLL imports it. Set `LARECOMP_DATA_ROOT` to the repository's `build` folder and prepend `build` to PATH for DLL dependencies. Runtime-only changes do not require rebuilding the extracted game DLL.

## Existing QA

- `tests/tools/run-mirror-pair.ps1`
- `tests/tools/run-ally-pair.ps1`
- `tests/tools/run-p0-clone.ps1`
- `tests/tools/ally-ui-smoke.py`
- `tests/tools/battle-mirror-link-test.c`
- `tests/tools/battle-mirror-relay-test.py`
- Other battle, party validation and relay tests under `tests/tools/`.
- `tests/tools/party-fixtures/` contains a PKHeX-based C# fixture helper.

Use unique run names and available relay ports. Inspect scripts before running; some older scripts copy `build/LetsGoRecomp.exe` rather than the isolated executable.

The last completed QA processes from `mirror-native1` were stopped after verification. Recheck current processes rather than assuming the machine is idle.

The `tests/` directory is ignored and is local evidence, not a checked-in test suite. Verify these artifacts exist when resuming elsewhere; do not claim old test results were independently reproduced if the artifacts are unavailable.

## Working style

Keep the user informed with concise, concrete updates at least every minute. Distinguish implemented code, passing automated tests and unresolved limitations. Continue through the work rather than stopping after a plan or asking for manual tests.
