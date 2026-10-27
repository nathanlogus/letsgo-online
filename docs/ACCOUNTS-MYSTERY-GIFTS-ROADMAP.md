# Accounts and Mystery Gifts: roadmap and agent handoff

Prepared 27 October 2026. This is the continuation roadmap for account login, profiles and Mystery Gifts. It supplements the general [project roadmap](ROADMAP.md) and [co-op roadmap](COOP-ROADMAP.md); it does not replace their scope or authorize shared progression.

## Start here

Read `AGENTS.md` and `CLAUDE.md`, inspect `git status`, and then read:

1. [Launcher account implementation](ACCOUNTS-LAUNCHER.md).
2. [Backend setup, endpoints and operations](../server/accounts/README.md).
3. [Architecture and save transaction design](ACCOUNTS-MYSTERY-GIFTS.md).
4. For relay or battle interactions, [co-op online notes](COOP-ONLINE.md), [battle prototype](COOP-BATTLE-PROTOTYPE.md) and [co-op handoff](COOP-AGENT-HANDOFF.md). Reconcile historical descriptions against current code and newer evidence.

The working tree contains unrelated co-op, rendering, day/night and settings work. Preserve it. Account/backend/launcher work from this session is also uncommitted; do not assume it exists in another checkout until checking. No commit, push or public deployment was requested.

**Next: A04b native item insertion and gift receipt.** A01–A03 are implemented: profile binding, inbox, code/gift preview, confirmation and encrypted retry-safe entitlement allocation. A04a is implemented: the native save is now replaced atomically (old or new, never torn or empty), tested on copied saves at every crash/write-failure boundary. A02V was investigated: raw PNG text masks did not support the earlier missing-letter report; an independent ImGui buffer-lifetime mismatch was corrected. Native reward application and acknowledgements remain disabled until A04b passes on copied saves.

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
| A04b | P1 / delivery gate | Map native item validation/insertion (bag add and capacity check) and add the gift receipt bound to the save generation (design below). Resolve transaction ordering before enabling reward application. | Forced termination and disk-write failure at every prepare/apply/save/receipt boundary recover deterministically; unrelated save state is unchanged; a duplicate packet cannot apply another reward |
| A05 | P1 / depends on A04 | Connect pending item claims to supported native delivery and acknowledgement. Explain full bag, wrong profile/build and recovery states in the UI. | Repeated delivery/ack, restart, full capacity, pause/expiry and disconnect cases neither lose nor duplicate rewards in supported clients; capture visible results and compare full relevant save state |
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

## Suggested next-agent instruction

> Continue accounts and Mystery Gifts using this roadmap. A01–A03 and A04a (atomic native save) are implemented; start with A04b: find the native bag-add/capacity functions and add the generation-bound gift receipt in `save_replace`, using isolated copied-save experiments and `tests/scripts/run-save-atomic.sh`. Keep delivery disabled until the save gate passes. Preserve unrelated renderer/co-op work and shared Keycloak policies; do not overwrite manual saves, log private codes/tokens, or enable cross-save progression implicitly. Record precise crash/write-failure coverage before claiming native delivery safety.

On every handoff, update this file with completed IDs, source changes, precise tests/artifact paths, unresolved limitations and the next concrete task. Keep updates concise during extended work and do not describe prototype or partial state checks as completed delivery/synchronization proof.
