# Accounts and Mystery Gifts: roadmap and agent handoff

Prepared 27 October 2026. This is the continuation roadmap for account login, profiles and Mystery Gifts. It supplements the general [project roadmap](ROADMAP.md) and [co-op roadmap](COOP-ROADMAP.md); it does not replace their scope or authorize shared progression.

## Start here

Read `AGENTS.md` and `CLAUDE.md`, inspect `git status`, and then read:

1. [Launcher account implementation](ACCOUNTS-LAUNCHER.md).
2. [Backend setup, endpoints and operations](../server/accounts/README.md).
3. [Architecture and save transaction design](ACCOUNTS-MYSTERY-GIFTS.md).
4. For relay or battle interactions, [co-op online notes](COOP-ONLINE.md), [battle prototype](COOP-BATTLE-PROTOTYPE.md) and [co-op handoff](COOP-AGENT-HANDOFF.md). Reconcile historical descriptions against current code and newer evidence.

The working tree contains unrelated co-op, rendering, day/night and settings work. Preserve it. Account/backend/launcher work from this session is also uncommitted; do not assume it exists in another checkout until checking. No commit, push or public deployment was requested.

**Next: finish A05 (UI states, Pikachu), then A06.** A01–A04 are implemented, and A05 item delivery works end to end in the launcher harness: a claimed gift is handed to the game, applied at the next native save with a receipt, and acknowledged to the server with the save hash. Missing for A05: player-facing states (full bag, quarantine, delivered) in the launcher, Pikachu addresses, and a server-side item allowlist. A02V was investigated: raw PNG text masks did not support the earlier missing-letter report; an independent ImGui buffer-lifetime mismatch was corrected.

## Current state

| Feature | Status and limits |
| --- | --- |
| Windows launcher Account page | Implemented: browser sign-in, signed-in identity, Remember me, cancellation, retry and sign-out |
| Keycloak integration | Uses existing `johto` realm with dedicated public client `letsgo-native`; no separate Let’s Go realm was created |
| Native authentication | Authorization code + PKCE S256, state validation and loopback callback; API verifies signed provider tokens and nonce |
| Credential storage | Windows Credential Manager stores the refresh credential; access credentials stay in memory |
| API account/session service | Implemented: issuer/subject identity, rotating refresh credentials, revocation and ownership checks |
| Profile binding | Launcher links edition + save directory to the signed-in account; separate per-service/account metadata preserves identity across restarts without changing native save files |
| Campaigns, codes and claim ledger | Implemented and tested against PostgreSQL: immutable rewards, finite stock, code-use limits, once per account/campaign and retry-safe allocation |
| Delivery acknowledgement | Implemented as a **client-reported** receipt, not verified game delivery |
| Gift inbox | Launcher Mystery Gift page implemented: available/pending/received, profile filtering, six-row pagination, refresh and signed-out/empty/loading/error states |
| Code entry / claim UI | Implemented: preview, explicit destination confirmation, encrypted retry journal, restart/resume and explicit local retry clearing; no native reward writes |
| Native reward application/recovery | Not implemented; `save_delivery_enabled` remains false |
| Staff authorization | API role and recent-MFA checks implemented; actual shared Keycloak staff MFA flow is not configured/verified by this work |
| Account-based co-op | Not implemented; existing guest room-code behavior is unchanged |
| Hosting | Local development Docker services only; public HTTPS hosting is not configured |
| Other platforms | Login is Windows-only; Linux reports it unavailable |

OpenMMO informed the service separation. It was inspected at `a0876357961f383de1c54fc71a7e3cd4b2a1fe58`; no implemented Mystery Gift redemption service was found there. Its client-compatible SHA-1 password scheme was not adopted. This project delegates passwords to Keycloak.

## Source map

| Location | Responsibility |
| --- | --- |
| `runtime/account.c`, `runtime/account.h` | Worker lifecycle, HTTP, PKCE/state generation, callback, refresh, Windows credential storage and public UI state |
| `runtime/account_data.inc` | Private worker profile metadata/binding and inbox jobs, cache invalidation and bounded row parsing |
| `runtime/account_claim.inc` | Preview/claim worker jobs and versioned per-user DPAPI-protected retry journals |
| `runtime/ui/launcher_lg.mm` | Account tab, browser opening and explicit launcher QA hooks |
| `Makefile` | Native account system libraries, JSON-header dependency and release license inclusion |
| `third_party/jsmn/` | Pinned MIT-licensed JSON tokenizer, with license/provenance |
| `server/accounts/app.py` | Authentication/profile/gift/staff routes, ownership, request limits and transaction boundaries |
| `server/accounts/auth.py` | OIDC/JWKS verification, discovery/internal routing and application credential issuance |
| `server/accounts/config.py`, `models.py` | Environment configuration and bounded request/reward schemas |
| `server/accounts/db.py`, `migrations/` | PostgreSQL pooling and serialized checksum-verified migrations |
| `server/accounts/__main__.py` | Explicit migration, staff-role grant, session revocation and rate-bucket cleanup commands |
| `server/accounts/keycloak_setup.py` | Idempotent dedicated-client setup; refuses incompatible existing settings |
| `server/accounts/compose.yml`, `Dockerfile` | Independent local API/database deployment |
| `runtime/services/fs.c` | Native save lifecycle that must be understood before durable gift delivery |

The account worker currently stops when the launcher closes or starts the game. Remembered credentials can resume on the next launcher visit. An in-game inbox needs an explicit account/session lifecycle and worker/API interface; do not assume the launcher leaves an authenticated worker running during gameplay.

## Existing local environment

- Repository: `C:/Users/Nathan/orca/letsgo-recomp`.
- Shared Keycloak Compose source: `C:/Users/Nathan/orca/ssrecomp/server/docker-compose.yml`.
- Keycloak: `http://localhost:8080`; issuer `http://localhost:8080/realms/johto`.
- Client: `letsgo-native`; API audience: `letsgo-api`; exact callback: `http://127.0.0.1:7386/callback`.
- Dedicated account Compose project: `letsgo-accounts`, sourced from `server/accounts/compose.yml`.
- API/docs: `http://127.0.0.1:7385/docs`; PostgreSQL has separate private storage.
- Private local environment: `tests/config/letsgo-accounts.env`. Never copy its contents into docs or logs.
- Docker server routes to Keycloak through `host.docker.internal`; public token issuer and browser authorization URL remain `localhost`. See `ACCOUNT_OIDC_INTERNAL_URL` and the backend README.
- Native API override: `LARECOMP_ACCOUNT_URL`; default is local port 7385. HTTP is restricted to loopback development, remote services require HTTPS.

Recheck running containers, ports, executable paths and process IDs before work. Do not restart the other project's Keycloak, account server or relay to simplify testing. Do not recreate its realm, overwrite `johto-online`, change shared authentication policies, or delete existing volumes. A new isolated staff client/flow is preferable to changing every player's shared browser flow.

The tested runtime was copied to `build/LetsGoRecomp.exe` while that path was idle. Its predecessor is preserved under the final QA run. Recheck which binary is current before replacing anything; manual game sessions may have started since this handoff.

## Ordered implementation backlog

Priorities describe remaining work, not confirmed defects. Each acceptance gate needs its own automated evidence before being marked complete.

| ID | Priority / state | Work | Acceptance gate |
| --- | --- | --- | --- |
| A01 | Complete: launcher routing profile | Explicit Link action binds edition + directory; account/service-specific metadata is written atomically before the API call; remote ownership is verified on restore. | Real launcher restart restored the same UUID; other directories/accounts received different UUIDs; backend ownership rejection passed; original saves/settings stayed unchanged |
| A02 | Complete: launcher inbox | Available/pending/received, owned-profile filter, pagination, refresh, cache invalidation and signed-out/empty/loading/error UI. | API/native tests passed; private account history stayed isolated; browsing left the ledger unchanged |
| A02V | Investigation closed; ring invariant corrected | Raw source-PNG title/sidebar masks were intact, so the earlier missing-letter report was unsupported. Separately, ImGui's host-written buffer ring now covers all ten permitted window frames in flight. | Source-PNG comparison and 13-case native regression evidence below; no claim that the ring change caused or cured the earlier visual interpretation |
| A03 | Complete: server entitlements | Code/campaign preview, explicit claim confirmation, frozen destination/adapter/key, encrypted journal before POST and explicit restart/resume. | Real launcher recovered the same claim after a proxy dropped a committed reply; one code use/allocation/claim remained; changed destinations conflict; no game reward write or ack |
| A04a | Complete: atomic native save | Mapped the game's save: `SetSize(0)`, one 1 MB `Write` of `savedata.bin`, `Flush` x2, `Commit`. It was done in place, so a crash or full disk left an empty/torn save. `fs.c` now defers a save file's SetSize and writes a whole-file Write to a flushed `savedata.bin.tmp` that replaces the file (`plat_replace_file`: `MoveFileEx` write-through / `rename` + directory fsync). | Copied-save runs for no fault, write failure, crash before replace and crash after replace; both crash leftovers reload and save again (see A04a evidence) |
| A04b | Complete on copied saves (QA-driven) | Item adapter, save-image checkpoint, gift receipt and startup recovery in `runtime/hle/gift.c` + `fs.c` `save_replace` (see A04b sections). | `tests/scripts/run-gift-receipt.sh`: commit, duplicate claim, crash before the receipt, after the receipt, after the replace, recovery of each, tampered receipt quarantined; one application per claim and only the bag changed in every case |
| A05 | In progress: delivery works (Eevee) | Launcher hands the oldest allocated claim of the linked profile to the game (`runtime/account_delivery.inc`), the game applies it at its next save (`gift.c`), the launcher acknowledges committed claims with the save hash. Remaining: full-bag/quarantine/delivered UI, Pikachu, server allowlist, repeated-ack and service-down cases in the harness. | Repeated delivery/ack, restart, full capacity, pause/expiry and disconnect cases neither lose nor duplicate rewards in supported clients; capture visible results and compare full relevant save state |
| A06 | P1 / planned | Configure and test a dedicated Keycloak staff MFA flow and usable campaign administration workflow. Keep API administrator roles explicit. | Actual second-factor login succeeds; password-only/stale-MFA sessions fail; audit events exist; existing Johto login behavior is unchanged |
| A07 | P1 before public release | Configure public HTTPS issuer/API addresses, exact redirects, proxy trust, separate staging settings and production deployment credentials. | Clean-client browser login and certificate validation pass; untrusted forwarded headers and wrong audience/issuer fail; databases stay private; no development credentials are shipped |
| A08 | P2 / planned | Rehearse backup/restore, ledger reconciliation, secret rotation, account disable, session revocation and data-retention/deletion procedures. | Restore does not reopen previously consumed entitlements; rotation has a documented migration path; immediate API disable/revocation is demonstrated |
| A09 | P2 / later | Add account-scoped invitations/identity to the relay with short-lived single-use tickets and a versioned encrypted transport. | Wrong audience, replay, expiry, disconnect/reconnect and room access tests pass; guest room-code Social play remains available; no refresh credential reaches the current plaintext relay |
| A10 | P2 / later | Add Pokemon gifts as a separate validated schema/adapter after item delivery is reliable. | Edition/build legality, capacity, randomized reward freezing and crash/retry ownership are tested; existing item delivery passes regression checks |

Broader follow-ups: actual realm registration/recovery UX, identity-provider back-channel logout, account linking/deletion, Linux secure credential storage and release-package testing on clean Windows. Provider logout/disable currently does not instantly revoke an already issued application session; API revoke/disable operations do.

## Save and entitlement boundaries

The implemented local routing identity is **edition + canonical save-directory path**, not a parsed native trainer identity. Replacing/importing a save into the same directory keeps that directory's binding. Separate directories/accounts get separate bindings. Profile rename, forget/rebind and save-import detection UI are not implemented. Do not describe the current mapping as verified ownership of the native save or a cloud-save feature.

- Social mode and independent progression remain the default. Login, profile binding or joining a room must not import another player's save, story flags or rewards.
- The database can enforce one **entitlement** per account/campaign. It cannot prove economy-wide uniqueness across editable, cloned or restored local saves. Do not claim anti-cheat or exactly-once game delivery from a unique SQL row or acknowledgement alone.
- Rewards and receipt identity must refer to the same durable save generation. An unrelated sidecar written after a normal save leaves a crash window; simply observing filesystem Commit is not proof of durability.
- `runtime/services/fs.c` now publishes each game save atomically (A04a). The game still aborts itself (`svcBreak`, result 2002-4000) on any save-write error; the old save is intact on disk when that happens. Commit stays a no-op because each whole-save Write is already durable.
- The current reward schema bounds item IDs/quantities but does not check a native game-version item allowlist. `lgpe-items-v1` is a contract label, not a finished delivery adapter.
- A claim freezes its reward and destination. Acknowledgement timeout must not automatically release stock, mint another claim or grant another reward. Campaign pause/expiry stops new allocations but does not erase pending recoverable claims.

## Evidence and reproduction

Launcher-login baseline: `tests/runs/account-gift-api-20261027-125641-87add3/` (12 passed). Profile/inbox functional run: `tests/runs/account-gift-api-20261027-134840-a69160/` (13 passed). A final profile/inbox capture run and its evidence are recorded in the continuation status below.

| Evidence | Recorded result |
| --- | --- |
| `pytest.log`, `junit.xml`, `result.json` | All 12 API/launcher scenarios passed |
| `keycloak-verification.json` | Real Keycloak PKCE flow, token checks and unchanged existing Johto client |
| `launcher/verification.json` | Actual native launcher sign-in, credential restore, sign-out, session-only behavior, callback/error paths and unchanged original save/settings hashes |
| `launcher/signin/*.png`, `launcher/restore/*.png`, `launcher/session-only/*.png`, `launcher/timeout/*.png`, `launcher/unavailable/*.png` | Captured UI states, visually inspected |
| `resources.json`, `api-container.json`, launcher verification process records | Exact owned resources; QA containers/processes were cleaned up |
| `database.sql`, `api-docker.log`, `postgres.log` | Isolated database and service evidence |
| `tests/logs/account-native-probe.log` | Native URL/JSON/Unicode/callback checks and preservation of rotated credentials during shutdown passed |

The suite also exercises account ownership, invalid/expired JWTs, refresh replay, concurrent allocation/code/stock races, rollback after injected failure, request limits and API process restart persistence. It emits one upstream Starlette/httpx test-transport deprecation warning. These results do **not** prove native gift delivery, staff MFA configuration, public hosting, full game synchronization or clean release installation. Browser authentication was scripted against real Keycloak forms; opening the user's default browser was suppressed in QA.

All harnesses, fixtures, private configuration, binaries and evidence under `tests/` are git-ignored and local. In a fresh checkout, verify they exist; recreate missing harnesses under `tests/` instead of asking the user to run routine tests or citing unavailable evidence as reproduced.

Build and run commands:

```powershell
# First inspect processes. Use the configured MSYS2 shell and preserve the executable name.
& C:/msys64/usr/bin/bash.exe -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /c/Users/Nathan/orca/letsgo-recomp && make -j4 B=tests/build-account-claims tests/build-account-claims/LetsGoRecomp.exe'

# Rebuild the local API image when backend code changes; follow the README for migration/startup.
docker compose --env-file tests/config/letsgo-accounts.env -f server/accounts/compose.yml build api

# Disposable PostgreSQL/API resources, real existing Keycloak and copied-profile launcher QA.
tests/venvs/account-gift-api/Scripts/python.exe tests/scripts/run-account-gift-checks.py --keycloak-env C:/Users/Nathan/orca/ssrecomp/server/.env --launcher-exe tests/build-account-claims/LetsGoRecomp.exe --container-api-image letsgo-accounts-api:latest
```

Relevant harnesses: `tests/scripts/run-account-gift-checks.py`, `tests/tools/test_account_gift_api.py`, `tests/tools/test_launcher_account.py`, `tests/tools/account-native-probe.c`. Native runs need `LARECOMP_DATA_ROOT` pointing to `build`, `build` in PATH, and independent save/settings/log/capture overrides. Use a unique run directory and credential namespace each time; never point automation at the player's save or `build/larecomp.ini`.

Profile/inbox QA uses `tests/build-account-profiles/LetsGoRecomp.exe` instead of the baseline login build in the example command. Set `LARECOMP_ACCOUNT_PROFILE_DIR` to the run's metadata directory so QA never creates bindings in the player's data root. The profile/inbox harness now also checks account switching, distinct directories, remote binding restoration, page boundaries and a paused **owned QA API container** for loading/error/retry coverage. Do not pause the normal local API or shared Keycloak to reproduce that test.

For future UI work, the exported asset gallery is `tests/runs/ui-assets-export-20261027-01/index.html`: 3,822 named textures across 51 menu groups, 3,726 matching launcher-cache 2x versions, original-resolution PNGs, transparent mask variants, manifests and contact sheets. These are local game-derived assets, not checked-in distributable resources. Design at 1920x1080, check readability at 1280x720, and keep text/panels separate for scaling.

## Claim implementation notes for the next agent

- Native session credentials remain private to the worker. Extend its job/snapshot interface; do not expose tokens to ImGui or write them into profile metadata.
- Binding metadata lives under `<data root>/account-profiles/` (override `LARECOMP_ACCOUNT_PROFILE_DIR`), keyed by service/account/edition/canonical-directory identity. It contains public IDs and labels only. The worker locks metadata creation and uses flush + atomic replacement before posting a new binding.
- `GET /v1/profiles/{id}` verifies own binding. `GET /v1/inbox?kind=available|pending|received&limit=6&offset=N&profile_id=UUID` projects titles, item counts and profile labels. Profile ID is optional for account-wide browsing, but A03 should require a verified explicit destination before allocating anything.
- `POST /v1/gift-preview` accepts the same code/campaign/profile/adapter selector as claiming. It validates ownership, returns preview/eligibility and consumes no code/stock. Eligibility is a point-in-time preview; allocation rechecks it transactionally.
- Claim jobs freeze destination, adapter, normalized code/campaign, reward digest and idempotency key. Before POST, `runtime/account_claim.inc` persists a version-1 bounded binary record protected by per-user Windows DPAPI, with service/account/profile entropy, under the public profile metadata path plus `.claim`. It uses a lock, flush and atomic replacement. The extra system library is `crypt32`.
- Private codes never enter plaintext profile JSON or ordinary logs. The retry record contains no password/access/refresh credential. Its current version validates record size and every bounded field; a future format/layout change needs a compatible reader or migration, not silent reinterpretation.
- Opening the launcher detects pending retry metadata but does not post automatically. Explicit Resume repeats the original request/key; valid allocation replies must match destination and reward digest before the journal is deleted. An uncertain/failed reply preserves it. Explicit Clear local retry only deletes the local journal; server claims/history remain untouched.
- The current inbox displays aggregate item counts, not resolved in-game item names/icons. Add those only with a validated item/version mapping. Received entries are explicitly client-reported; all such launcher QA entries were controlled database fixtures, not native reward delivery.
- Selection/account changes invalidate profile and inbox caches using generation counters. Preserve this when claim jobs are added, including slow-response and logout races. Keep the existing encrypted credential scope and callback behavior intact.
- Profile restore can fail on invalid/unreadable metadata; it does not silently replace the saved UUID. Error/rename/rebind polish remains future work; do not clear backend entitlements to solve local binding problems.

## Profile/inbox continuation status (27 October 2026)

Source/build: profile/inbox code is in `runtime/account_data.inc`, `runtime/account.c/.h`, launcher UI and `server/accounts/app.py`; isolated runtime is `tests/build-account-profiles/LetsGoRecomp.exe`. No new database migration is required. Backend additions are owned `GET /v1/profiles/{id}` and read-only `GET /v1/inbox`.

Final run: `tests/runs/account-gift-api-20261027-135253-2ffde8/`, **13 tests passed**, with the same upstream Starlette/httpx deprecation warning. `launcher/verification.json` records persistent binding restore, separate directory/account UUIDs, filtered private history, pagination, empty/loading/error states, cleared logout caches and unchanged claim ledger/original saves/settings. `tests/logs/account-profiles-native-probe.log` records native parsing, UUID/label/query handling, cache clearing and credential-shutdown checks. The earlier functional run is preserved at `account-gift-api-20261027-134840-a69160`.

Visually reviewed captures include `launcher/signin/profile-linked.png`, `inbox-available.png`, `inbox-pending.png`, `inbox-received.png`, `inbox-service-error.png`, plus `launcher/account-switch/account-switch-private-history.png` and `launcher/session-only/inbox-empty.png`. An initial visual interpretation suggested missing glyphs; the later raw-pixel check below did not support that report. This historical observation is superseded by the latest investigation.

The QA API/PostgreSQL containers, temporary Keycloak users, credential namespace and launcher processes were cleaned up. Read-only inbox history was populated with explicit isolated database fixtures; no native reward delivery was performed. Profile bindings follow directories, not a parsed trainer/save identity. A03 allocation UI, A04/A05 delivery and staff/public-hosting work remain open. Inspect current processes before updating binaries or resuming QA; do not stop another agent's battle runs.

The idle normal `build/LetsGoRecomp.exe` was updated from that tested profile/inbox build; its predecessor is preserved in that run. The local API was updated without a database migration. Newer claim/rendering evidence and build details are recorded below.

## Latest continuation: A02V and A03 (27 October 2026)

- Isolated runtime: `tests/build-account-claims/LetsGoRecomp.exe`. Main additions: `runtime/account_claim.inc`, claim UI/QA hooks, `POST /v1/gift-preview`, `crypt32` linkage and the shared `VK_WINDOW_FRAMES_IN_FLIGHT` constant in `vk.h`/`device.c`/`imgui_glue.cpp`.
- A02V evidence: `tests/runs/account-gift-api-20261027-141809-4bd49a/static-text-comparison.json` compares the original source PNGs against new captures. Account/Mystery Gift/Graphics sidebar dark-pixel masks match; title variation is at most 0.26% at antialiased edges. The previously reported missing letters are not present as glyph losses in those raw crops. Standalone repeated-title and tab-switch capture runs also did not reproduce glyph loss. Do not keep reporting this as a confirmed visual defect.
- Independent renderer hazard: the CPU window frame ring was ten, while the ImGui backend recycled vertices/indices after approximately four draws. Both now share the ten-frame constant, preserving the surrounding renderer work. The native 13-case regression passed with that change. This establishes the lifetime invariant; it does not establish the cause of the earlier visual interpretation or GPU-wide gameplay compatibility.
- Final A03 run: `tests/runs/account-gift-api-20261027-143256-dfd461/`, **14 tests passed**, one upstream Starlette/httpx warning. `launcher/verification.json` adds non-consuming preview, encrypted retry persistence, recovery after a lost committed response and restart, and duplicate-claim protection. The test proxy forwarded the real claim to an isolated Docker API, discarded its successful reply, then allowed explicit restart/resume; database checks confirmed one entitlement and one code use.
- Visually inspected: `launcher/signin/claim-preview.png`, `claim-lost-response.png`, and `launcher/restore/claim-recovered-after-restart.png`. `claim-title-pixels.json` confirms identical title masks across those states. Clear-local-retry is implemented but its full GUI interaction is not separately automated; the main covered recovery path is Resume.
- Native probe: `tests/logs/account-claims-native-probe.log` passes parsing, UUID/label, cache invalidation, credential shutdown and real DPAPI pending-record roundtrip/wrong-account rejection checks. Protected probe artifacts stay under `tests/runs/claim-probe-*/`.
- The first full A03 run (`account-gift-api-20261027-142913-f65e7d`) reached all feature assertions but failed QA cleanup because its Keycloak admin token expired. Cleanup now obtains a fresh token, the two exact owned fixtures from that run were removed, and the final full run passed. Do not confuse that QA cleanup issue with player authentication or gift allocation failure.
- No native reward application, save transaction implementation or delivery acknowledgement was added. Item previews still show aggregate counts, not resolved item names. Staff MFA/public hosting remain open. The next implementation task is **A04**, with copied-save crash/write-failure evidence before any real save delivery.
- The local API was updated to the tested preview/claim image (no schema migration) and the idle `build/LetsGoRecomp.exe` was updated from the tested isolated claim build. SHA-256 hashes matched. Its predecessor is preserved in the final run as `previous-LetsGoRecomp.exe`; `deployment-smoke.json` records readiness, native configuration and the preview route. QA resources were removed without stopping other agents' game sessions. All source changes remain uncommitted.

## A04a: atomic native save (27 October 2026)

- Source: `runtime/services/fs.c` (`save_settle`, `save_replace`, save logging), `plat_replace_file` in `runtime/platform.h`, `platform/windows.c`, `platform/linux.c`. Isolated build: `tests/build-save-atomic/LetsGoRecomp.exe`; `build/LetsGoRecomp.exe` was not replaced (a game session was running).
- Mapped sequence (log `[save] set size/write/flush/commit`): `SetSize(0)` -> `Write(off 0, 1048576)` -> `Flush` -> `Flush` -> filesystem `Commit`. Before this change the SetSize truncated the only save to 0 bytes on disk.
- Harness: `tests/scripts/run-save-atomic.sh <run> [none|fail|crash-copy|crash-replaced]` (`SRC=<dir>` starts from another save folder) saves the copied fixture `tests/saves/save-atomic/` through the in-game menu and writes `result.txt` (hashes, leftover `.tmp`, save log). `LARECOMP_SAVE_FAULT` injects the fault.
- Results: `tests/runs/save-atomic-none4` (new image, no `.tmp`), `save-atomic-fail4` (save byte-identical, game aborts on the write error as before), `save-atomic-crash-copy4` (old save byte-identical + full `.tmp`), `save-atomic-crash-replaced4` (full new image). Reload runs `reload-after-crash-replaced1` and `reload-after-crash-copy1` load those leftovers (`tests/saves/after-*`) and save again; `fin_1800.png` of the first shows "You saved your progress!" with the crashed run's "Last saved" time.
- Not covered: real disk-full/permission errors (only the injected failure), power loss below the OS, a crash *during* `MoveFileEx`, macOS/Linux runs.

### A04b receipt design

The runtime now owns the moment a save becomes durable, so the receipt can be bound to it without a crash window:

1. Apply the reward in memory only at an idle checkpoint, with the claim journaled as `applied, awaiting save` (claim ID, SHA-256 of the current `savedata.bin`).
2. In `save_replace`, after the `.tmp` is flushed and before the replace, atomically write the receipt sidecar: claim ID, previous-image hash, new-image hash.
3. Startup recovery, before gameplay: `savedata.bin` hash == new hash -> committed, send/repeat the ack. == previous hash -> crash before the replace, the reward is not in the save: still pending, may be applied again. Neither (the file was changed outside the game) -> quarantine with a recovery message, never re-grant. Recovery must finish before the game can save again, or a later save would hide a crash-before-replace. Restoring an older backup defeats this; that is the accepted local-save boundary.
4. The ack never triggers another application; a receipt only moves from committed to acknowledged.

## A04b progress: native item adapter (27 October 2026)

- `runtime/hle/gift.c` (Eevee build only; `gift_configure` in `runtime/main.c`): `LARECOMP_GIFT_QA=<item>:<count>` adds once on the world's 30th update; `LARECOMP_GIFT_QA=table` logs the game's item table. Nothing else calls it; delivery stays disabled.
- Native map: bag add `0x70001c8e20(bag, item, count, mark_new)`; bag = world (`[[0x70015fad08]]`) `+0x98` -> `+0x58` -> `+0x68`; pocket = `0x700071d340(item, 0xb)` = item byte bits 0-2 (table `[0x700163ea48]`, ids 0..0x421); pocket tables at `bag+0xde8+8p`, 60/108/200/150/50/150/150 u32 slots (item 0-14, count 15-29, new 30). Pockets: 0 medicine, 1 TMs, 2 candy, 4 balls, 6 items + key items. Key items have item-byte bit 7 (`0x700071d3a0`).
- The game validates nothing: the add caps a stack at 999 silently, ids above 0x421 go to pocket 0, and unused ids are stored as they are. `gift.c` therefore refuses anything outside a 207-item allowlist (PKHeX's legal Let's Go items from `tests/tools/gift-items`, minus TMs and key items) and anything that would not fully fit, before calling the native add; it then checks the held count rose by exactly the gift.
- Evidence (copied save `tests/saves/save-atomic`, saved through the menu, `save-diff.txt` in each run): `gift-qa1` Potion x3 `item 17: 0 -> 3`, only `MyItem` changed besides PlayTime/position noise; `gift-qa-refuse2` x1000 refused (no room), save PASS; `gift-qa-refuse3` id 5000 and `gift-qa-refuse4` Secret Key 872 refused (not giftable), save PASS. Before the allowlist, id 5000 was accepted and stored (`gift-qa-refuse1`, read back as item 904). Item table dump: `tests/runs/gift-table1/table.txt`; allowlist source: `tests/logs/gift-allowlist.txt`.
- Superseded by "A04b: save checkpoint and receipt" below: the QA trigger now applies at the save checkpoint, not on the 30th world update.

## A04b: save checkpoint and receipt (27 October 2026)

- Save path (found with a temporary guest stack log in `fs.c` and `LARECOMP_WATCH` on the image buffer, runs `save-trace1`/`save-trace2`): the save menu (`0x700097b400`, game thread) calls `0x70001cddf0(save_obj)`, which memsets and fills the 1 MB image at `save_obj+0x90` from world`+0x98`; a worker job (`0x70001cebe4`) then does `SetFileSize`, `WriteFile(image, 0x100000)`, `FlushFile`, `CommitSaveData` (SDK PLT `0x7000d31ee8`/`0x7000d31f18`/`0x7000d300c8`).
- `gift.c` hooks `0x70001cddf0` (chained): before the image is built it adds the pending claim's item to the bag (allowlist + room checks as before), unless the claim is already committed or the receipt is quarantined.
- Receipt `<save>.gift` (text): `committed <claim>` lines plus, during one replace, `pending <claim> <old sha256> <new sha256>`. `save_replace` order: flush `.tmp` -> write the pending receipt (flushed, atomic replace) -> replace the save -> record `committed`. A receipt write failure fails the save (old save kept, claim still pending).
- Recovery when the game first opens its save: pending + save hash = new -> committed; = old -> dropped, claim still pending; other or unreadable -> quarantine (no gifts). Committed claims are never applied again.
- New QA fault `crash-receipt`; `crash-replaced` now ends after the replace and before `committed` is recorded. QA trigger: `LARECOMP_GIFT_QA=<item>:<count>:<claim>`.
- Evidence: `tests/logs/gift-receipt.txt` (runs `tests/runs/gift-<step>1/`, saves kept in `tests/saves/gift-receipt/<step>/`), all PASS with only `MyItem` changed besides PlayTime/position: commit c1 `item 17: 0 -> 3`, receipt `committed c1`; duplicate c1 from that save: not applied, still 3; crash-copy: save and receipt unchanged; crash-receipt: old save + `pending c3`, then recover-pending: "never reached the save: still pending", applied once, `committed c3`; crash-replaced: new save with the item + `pending c4`, then recover-committed: "reached the save ... committed", not applied again, still 3; quarantine (receipt from another save): no gift.
- Limits: QA trigger only (no claim from the API reaches the game, no acknowledgement); recovery runs only while a gift is configured (it must run on every launch once delivery exists); Eevee addresses only; one gift per save; a failed save write leaves the item in the running game's bag without a receipt, which is safe only because the game aborts on that error; Windows only (SHA-256 via BCrypt); the saving manager is used without the retain/release its callers do.

## A05: delivery from real claims (27 October 2026)

- Handoff, because the account worker stops when the game starts: when a profile is confirmed linked or a claim completes, the worker (`delivery_sync` in `runtime/account_delivery.inc`) (1) reads `<save dir>\save_0\savedata.bin.gift` and, for each `committed <claim> <save sha256>` whose claim is still `allocated` for this profile, POSTs `/v1/claims/{id}/ack` with that hash as `receipt_digest`; (2) writes the oldest allocated `lgpe-items-v1` claim of the profile that isn't committed to `savedata.bin.gift-inbox` (`claim <uuid>`, `item <id> <count>` lines; atomic write; removed when there is none). No credentials leave the worker; `account.c` still writes no logs.
- Game (`gift.c`): recovery now runs on every launch when the save is first opened, then reads the handoff; a claim's items (up to 32) are applied all or none (allowlist, 999 cap and free slots per pocket checked first); committed lines carry the new save's SHA-256. The serializer hook is always installed on the Eevee build; `LARECOMP_GIFT_QA` still overrides the handoff for QA.
- Launcher message after a claim: "It arrives in the game the next time you save."
- Evidence: `tests/logs/gift-receipt.txt` (11 steps, rerun after these changes, all PASS; new steps `inbox-multi` 2 items as one unit, `inbox-again` not reapplied, `inbox-noroom` nothing added and stays pending). Launcher harness `tests/runs/account-gift-api-20261027-172245-0bf94e/`: 14 passed; in the `restore` scenario the handoff named an allocated fixture claim of profile a, one scripted game run on that profile's copied save (`launcher/delivery-game.txt`) applied it (`launcher/delivery-save-diff.txt`: `item 4: 50 -> 55`, PASS), the next sync acknowledged it in the database with `receipt_digest` = the save's SHA-256, and the handoff moved to the next allocated claim (`verification.json`: `gift_handoff_to_game`, `gift_applied_at_native_save`, `gift_ack_with_save_hash`, `next_claim_handed_over`). `tests/scripts/run-save-atomic.sh` gained `SAVE=<dir>` (in-place run) for this.
- Limits: the handoff covers the first 100 claims of the account; one claim per save; a claim that doesn't fit stays pending with no player-facing explanation yet; acks are retried only on the next sync trigger (profile restore/link, claim completion); Eevee only; the server accepts any item id 1..65535 in a campaign.

## Native Mystery Gift investigation (27 October 2026)

Question: deliver claims through the game's own Mystery Gift (X > Communicate > Mystery Gifts > Get via internet) instead of adding items at save time. Findings, on the copied fixture save (`tests/runs/mgmenu1`..`mgmenu4`, `tests/scripts/run-save-probe.sh` with `EXE=`):

- The native screen works in the runtime: "Would you like to receive a Mystery Gift?" with Get via internet / Get with code/password / Check Mystery Gifts (`mgmenu3/fin_1900.png`). Entering it opens `bcat:u` (cmd 0, a logging stub today). "Get via internet" submits a `nifm` request (IRequest cmds 11, 21) and waits at "Connecting... Please stand by." (`mgmenu4/fin_2350.png`), because `runtime/services/network.c` answers as an offline console.
- "Get via internet" is BCAT: the game imports `nn::bcat` (`RequestSyncDeliveryCache`, `MountDeliveryCacheStorage`, `EnumerateDeliveryCacheDirectory`, `DeliveryCacheDirectory/File Open/Read/GetSize`; PLT `0x7000d30488`..`0x7000d32708`, one wrapper each at `0x700011de1c`..`0x700011e580`). The wrapper enumerates every directory and file of the cache (no fixed names), so the game reads whatever the service serves. BCAT files are already decrypted by the system service, so the game parses plain card data.
- Card and receipt formats (PKHeX.Core 24.7.27 via `tests/tools/gift-items <save> members`): a card (`WB7`) is 784 bytes, card data from 0x208, item or Pokemon (`IsItem`, `ItemID`, `Quantity`, `AdditionalItem`; `Species`, IVs, moves...). The save keeps `GiftRecords` (`WB7Records`): the last 10 received cards (`WR7`) and a received-flag bit array of 4224 card ids (from 0xC80 in the records). That flag is the native receipt the design asked for: it lives in the save, so the atomic save (A04a) makes gift + flag one transaction.
- Not verified yet: whether the game checks anything beyond the card bytes (a signature, a list file, the card id range, the date window), the exact `bcat:u` / `IDeliveryCacheStorageService` command sequence it uses, which card ids official events used (ours must not collide), and whether "Get with code/password" can be served (it likely talks to Nintendo servers over `ssl`, not BCAT).

Proposed change (not implemented): keep the launcher handoff and the server ack; replace the save-time apply with (1) `nifm` answering "connected" only while a handoff is pending and the Mystery Gift screen asks, (2) an HLE `bcat:u` serving one directory with the pending claim as a `WB7` built by the runtime, (3) acknowledgement when a committed save has that card's received flag (read from the save, replacing the `.gift` receipt), with a campaign -> card id mapping on the server. Pokemon gifts (A10) would come with the same path, validated by the game. First step: the HLE `bcat:u` + `nifm` probe with one PKHeX-made item card on a copied save.

### Native Mystery Gift probe (27 October 2026)

- New, research only: `runtime/services/bcat.c` serves the delivery cache from `LARECOMP_BCAT_DIR=<folder>/<directory>/<file>` (bcat:u, IBcatService sync with an already-done progress, storage/file/directory services); with that variable `nifm` reports the request accepted and IGeneralService 21 `IsAnyInternetRequestAccepted` true (`runtime/services/network.c`). Without the variable nothing changes. `LARECOMP_BCAT_WATCH=<hex offsets>` arms `memwatch` on the file once it is in the game's buffer (`memwatch_watch`).
- Card: `tests/tools/gift-items card <out.wb7> <card id> <item> <count> [title]` (PKHeX WB7, `RestrictVersion` 3, today's date); probe folder `tests/saves/bcat-probe/` (`normal/card.wb7`, empty `zone/`).
- Results (copied fixture save, X > Communicate > Mystery Gifts > Get via internet; `tests/scripts/run-save-probe.sh` with `EXE=` and `GAME_LIB=`): empty cache -> the game's own "No gift was found." (`tests/runs/mgbcat2/fin_2650.png`). With the card: sync, open directory `normal` (twice), open and read `card.wb7` (784 bytes), open directory `zone`, then "There was a problem with receiving the gift." (`mgbcat3`..`mgbcat5`), with or without an empty `zone/` and with `RestrictVersion` 0 or 3. The empty case never opens `zone`, so `zone` is probably required data (a distribution/region list the card is checked against); its file names and format are unknown.
- The BCAT reader at `0x700011de1c` is the generic gflib3 loader (all files of a directory into containers); the card check is in the Mystery Gift code, not found yet. A read watch on the loaded card (`mgbcat7`) saw no guest reads before the buffer was reused: the game copies it with the runtime's memcpy.
- Two environment notes: runs occasionally stopped at boot with "extracted/lasgame.dll was built for another version" although the id file matches (passing `GAME_LIB=build/extracted/lasgame.dll` to the probe script avoided it); and builds failed twice on another session's in-progress edits (`coop_battle.c`, `textures.c`).
- Digests: `GetDigest` and the directory entries now carry the file's MD5 (as yuzu); the card is still refused the same way (`tests/runs/mgbcat6/`), so the digest was not the check. After `zone` the game calls `ITimeZoneService` 4 `LoadTimeZoneRule` (stubbed, the rest of the time calls are served as UTC): probably the card's date window, not yet ruled out.
- Run note: the "built for another version" boots were the environment being lost, not the library: starting `/c/msys64/usr/bin/bash` from Git's bash drops every variable (`LARECOMP_GAME_LIB`, `LARECOMP_BCAT_DIR`...), so the game booted the default library without them. Run the scripts with Git's `/usr/bin/bash` (or don't put `/c/msys64/usr/bin` first in `PATH`).
- **Solved (27 October 2026): the game's own Mystery Gift receives our cards.** What it needs, found with `LARECOMP_BCAT_TRACE=1` (hooks on the apply job `0x7000b2f5d0` and the directory loader `0x7000112530`) and a read watch on the loaded card (`LARECOMP_BCAT_TRACE_MARK=u:<title>` + `LARECOMP_WATCH_READ=1`):
  - Both directories `normal` and `zone` must hold at least one file (an empty one fails the load with result 0x47a: "There was a problem with receiving the gift."). Both are lists of cards: every file goes through the card parser `0x70001c7370(out, data, size)`.
  - The parser: size exactly 0x310; the u16 at 0x202 is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF; table built by `0x70001c88c0`) of the 0x310 bytes with that field zeroed. Then card id (0x208), type (0x258), item flag (0x259), items (0x270..), one-time flag (0x25a bit 0).
  - A one-time card is refused as already received when its received flag (save bitmap, 0x800 ids) is set, and **any card id above 0x7ff counts as received**: one-time card ids are 0..2047. A card with the flag clear is repeatable (no received flag, the same card can be received again).
  - `tests/tools/gift-items card` now writes the CRC and a one-time card; `<save> received <item> <card id>...` prints the item count, the save's card records and the received flags.
  - Result (`tests/runs/mgbcat6/`, kept in `tests/runs/mgbcat-results/`): "Please choose the gift you want to receive" lists both cards; receiving shows the game's own card ("Here's what you got: Potion x3"), the game saves (atomic replace); Potion 0 -> 4 (3 + 1), cards 1900 and 1901 flagged received, one record each, no duplicate.
  - Fixed on the way: `memwatch` read mode recursed forever (its log read the no-access word before making the page readable).
- Next: decide the delivery route. The native path gives the game's own screens, receipt (received flag + card record in the save, saved by the game) and duplicate protection, so the A05 save-time injection and its receipt could be replaced by serving each claim as a one-time card (card id from the claim, 0..2047, server-assigned) in the BCAT folder. Open: card id allocation, `zone` content (a second card is served today; one copy of the same card may do), Pokemon cards, Pikachu.
- **Native delivery, built (27 October 2026; game-side runs pending, the game was busy with profiling):**
  - Server: migration `002_native_cards.sql` (profiles.next_card_id 1024..2048, claims.card_id unique per profile). Adapter `lgpe-wb7-v1`: a claim with it gets the profile's next card id in the claim transaction (409 `profile_card_ids_exhausted` after 2047, rolled back); `card_id` is in the claim view. Reward kind `pokemon` (species 1-151/808/809, Alolan form 1 only for the 18 Alolan species, level, shiny never/random/always, nature, gender, 6 IVs with -1 random, 1-4 moves, ball), allowed only with the native adapter; a native item card holds at most 6 items. `/v1/gift-preview` returns the campaign's adapters. API QA: 15 passed (3 new tests), `tests/runs/account-gift-api-20261027-*`.
  - `runtime/wb7.c`: the card writer (items or a Pokemon; offsets from PKHeX via `gift-items layout`, CRC as the game) and the received-flag reader (save 0x45600 + 0xC80, bit id). Self-check `tests/tools/wb7_check.c` + `gift-items show` (PKHeX parses the cards and generates the Pokemon: trainer = the receiver, random IVs, Alolan shiny Raichu).
  - Launcher (`runtime/account_delivery.inc`): previews with `lgpe-wb7-v1`, falls back to `lgpe-items-v1` when the campaign has no native adapter; each allocated native claim not committed becomes `savedata.bin.bcat\normal\<card id>.wb7`, plus `zone\filler.wb7` (one-time id 0xffff: always skipped) and `cards.txt` (card id -> claim); stale cards are removed.
  - Game (`runtime/hle/gift.c`, both editions): reads `cards.txt`; a save whose image has a served card's received flag commits that claim ("committed <claim> <save sha>", the existing receipt and launcher ack); startup recovery commits cards found in the save on disk. `bcat.c` serves `<save>.bcat` by default; `nifm` is online only when there is something to serve.
  - To run: the native route end to end (launcher claim -> card -> receive -> save -> ack), a Pokemon card in game (does the game take an empty OT/TID as the player's, as PKHeX does), and whether a card served in `zone` also lists.
- **In-game "Get with code/password" (static findings):** the game first ensures a Nintendo account ID token (`EnsureNetworkServiceAccountIdTokenCacheAsync`), then POSTs `{"nuid":"%llu","rom":"%02d","serialcode":"%s","tok":"%s","langcode":"%02d"}` to `https://api.fushigi.switch1.pokemon-gl.com/serial.auth` (`serial.update` after receiving) through its HTTP wrapper (object +0x40: start 0x7000afe8e0, busy 0x7000afed50, done 0x7000afedb0, response 0x7000afedd0 {.., body at +8}, status 0x7000afedf0). It wants HTTP 200 and `"serialcodeStatus":"0"` (other numbers: an error code for the screen), then `"dataNo":"<=255 chars>"`, passed to the Mystery Gift code (likely a card of the `zone` data: `zone` is parsed with mode 2, `normal` with 1). To answer it from our API the account session has to stay alive in game (the launcher calls `account_shutdown()` before the game starts).
- **Gaps closed (27 October 2026, built and tested without the game; game runs pending):**
  - Card ids: official distributions PKHeX knows are 1-4, 501-503, 1001-1006, 2001 (`gift-items events`), so ours are 1024-1999. A save that already has a card id's flag (an unknown distribution, a save from elsewhere): before a card is first served the launcher checks the save and `POST /v1/claims/{id}/card` moves the claim to the profile's next id (only while the reported id is current). A card already served keeps its id (a set flag then means our card was received).
  - Server allowlist: the 207 giftable items (`GIFTABLE`, the game side's list) for both routes.
  - Launcher: inbox rows and the preview carry `species`/`level`/`native`; texts per route ("In the game: Mystery Gift > Get via internet" / "at your next save"); `<save>.gift-status` (gift.c: `no-room`, `not-giftable`, `quarantined`) shown as a note.
  - In-game codes: `account_enter_game()` keeps the worker when signed in with a linked profile; `runtime/hle/serial.c` answers requests to `api.fushigi.switch1.pokemon-gl.com` (no network): `serial.auth` claims the code as a native card through the worker and replies `serialcodeStatus` 0 + `dataNo` <card id>, or 1 invalid / 2 used / 3 unavailable / 9 failure; `serial.update` replies 0. `acc` IManagerForApplication 2/3 (ID token: placeholder, done). Open in game: what the game does with `dataNo` (a card of `zone` by name or id?) and which status numbers map to which message.
  - Dashboard `/admin/` (Keycloak sign-in through its own public client `letsgo-admin`, `/v1/auth/admin/*`; admin role + recent MFA as before): campaigns (items or Pokemon, publish/pause, codes, claims), accounts (search, disable), relay sessions (list by hash, close), audit. Artwork from a UI export + `assets/` via `dashboard/collect_assets.py` into a mounted folder, never in git or the image. Previews: `tests/runs/dashboard-preview/`.
  - Relay: `/rooms` and `/rooms/<id>/close` on its health port behind `COOP_RELAY_ADMIN_TOKEN` (`tests/tools/relay-admin-test.py`). Compose (`server/accounts/compose.yml`): `relay` service (`server/relay.Dockerfile`), and Keycloak under profile `identity`, reusing ssrecomp's `server_keycloak-db-data` volume (external) so the realm moves unchanged.
  - Tests: API 18 passed (`tests/runs/account-gift-api-20261027-210700-164827`), relay admin, wb7 self-check; `test_launcher_account.py` has a native-card case (collision -> 1025, card bytes, CRC, map, filler) to run with the game.
  - Not done here (operator steps): migrations on the live database, the `letsgo-admin` Keycloak client, moving Keycloak (stop the old containers, keep the volume), `RELAY_ADMIN_TOKEN`/`KEYCLOAK_*` in the env file.
- Older next options (superseded): find the `zone` consumer statically (strings/callers around the Mystery Gift app, the message for "problem with receiving"), or compare with a real Let's Go BCAT cache dumped from the user's own console. The save-time delivery (A05) keeps working meanwhile.

## Suggested next-agent instruction

> Continue accounts and Mystery Gifts using this roadmap. A01–A04 are implemented and A05 delivery works end to end for Eevee (launcher handoff file -> game applies at its next save with a receipt -> launcher acknowledges with the save hash). Finish A05: show delivered / waiting for a save / no room / quarantined states in the launcher Mystery Gift page, map the Pikachu addresses in `runtime/hle/gift.c`, add the 207-item allowlist to the server campaign validation, and extend `tests/tools/test_launcher_account.py` with full-bag and service-down ack retry. Re-run `tests/scripts/run-gift-receipt.sh` and the account harness after any change to `gift.c`, `fs.c` or `account_delivery.inc`. Preserve unrelated renderer/co-op work and shared Keycloak policies; do not overwrite manual saves, log private codes/tokens, or enable cross-save progression implicitly.

On every handoff, update this file with completed IDs, source changes, precise tests/artifact paths, unresolved limitations and the next concrete task. Keep updates concise during extended work and do not describe prototype or partial state checks as completed delivery/synchronization proof.
