# Launcher account sign-in

The Windows launcher now has **Account -> Sign in with browser**. It opens the existing Keycloak `johto` realm using the dedicated `letsgo-native` public client. Players enter passwords in Keycloak, then return to the launcher. The page shows connection progress, the signed-in name, cancellation, retry and sign-out. Solo play and room-code co-op remain available without an account.

After signing in, **Account -> Local profile -> Link this local profile** binds the selected game's save directory to the account, using an editable label. The **Mystery Gift** page browses available, pending and received gifts, filters to the linked profile and supports pagination/refresh. It now also previews campaigns/codes and explicitly confirms server claims, with an encrypted restart-safe retry journal. [Claim/recovery details](MYSTERY-GIFT-CLAIMS.md). Native gift delivery remains disabled.

Profile metadata is separate from native saves under `<data root>/account-profiles/`. Each service/account/edition/canonical-save-directory combination has its own random UUID, written atomically before a binding request and verified remotely on restart. QA overrides this with `LARECOMP_ACCOUNT_PROFILE_DIR`. The binding follows the directory: importing/replacing a save in the same folder keeps that binding. It does not attest native trainer ownership. Rename/forget/import detection UI is not implemented.

**Remember me** stores only the API refresh credential in Windows Credential Manager, scoped to the service and current Windows user. Access credentials stay in memory. Remembered sign-in is restored and rotated when the launcher opens again. With Remember me off, closing the launcher revokes the session. Sign out deletes the local credential and attempts server revocation; if the server is unreachable it still forgets this device's credential. Signing out of this app does not sign out other Keycloak applications.

The default local API is `http://127.0.0.1:7385`. Start the [account service](../server/accounts/README.md) and the existing Keycloak Docker stack first. `LARECOMP_ACCOUNT_URL` selects another API. Remote services require HTTPS with normal Windows certificate validation; HTTP is allowed only for explicit localhost/loopback development. There is no client secret, embedded browser or password field in the launcher. Linux currently reports sign-in unavailable rather than storing credentials insecurely.

## Protocol and lifecycle

- The native worker fetches `/v1/auth/native/config` without blocking UI rendering, creates cryptographically random state, nonce and PKCE verifier, and binds `127.0.0.1:7386` exclusively before opening the browser.
- Invalid paths, malformed/duplicate parameters, mismatched state and an unexpected issuer are rejected without consuming the pending sign-in. The listener has bounded reads, cancellation and a three-minute overall deadline. A busy port produces a recoverable error.
- The code, verifier and nonce go to `/v1/auth/native/complete`. The backend handles provider-token exchange and signature/nonce/audience checks; only revocable app credentials return to the runtime. The browser receives a fixed response with no reflected code or state.
- WinHTTP validates TLS, disables automatic redirects and implicit Windows authentication, and bounds HTTP response size and network waits. No tokens are logged. The native client uses the OS RNG, SHA-256 and Credential Manager APIs.
- API refreshes run serially. A rotated credential is saved before another network request. Closing during a successful refresh preserves the rotated saved credential; a lost refresh response may require signing in again because the server rejects refresh replay.
- Starting the game or closing the launcher stops its account worker and callback listener. Remembered sessions can resume next time. This does not yet authenticate the co-op relay or apply gifts to saves.

Code: `runtime/account.c/.h`, `runtime/account_data.inc`, `runtime/account_claim.inc`, the Account/Mystery Gift pages in `runtime/ui/launcher_lg.mm`, and endpoints in `server/accounts/`. JSON parsing uses pinned MIT-licensed jsmn; the release copies its license. Windows adds only system library dependencies (`winhttp`, `bcrypt`, `advapi32`, `crypt32`).

## Automated verification

Build in isolation while manual sessions run:

```bash
make -j4 B=tests/build-account-claims tests/build-account-claims/LetsGoRecomp.exe
```

Then run the existing account harness with the real launcher and an isolated container API:

```powershell
tests/venvs/account-gift-api/Scripts/python.exe tests/scripts/run-account-gift-checks.py --keycloak-env C:/Users/Nathan/orca/ssrecomp/server/.env --launcher-exe tests/build-account-claims/LetsGoRecomp.exe --container-api-image letsgo-accounts-api:latest
```

Build that API image with the Compose instructions first. The harness uses a disposable Keycloak user, independent PostgreSQL and API containers/ports, copied saves/settings and a unique credential-storage namespace. It checks actual launcher sign-in, remembered restart, sign-out revocation, session-only logout, wrong-state callbacks, cancellation, callback-port conflict, timeout and service-unavailable screens. It hashes original save/settings files before/after and deletes only its own QA credentials, user and processes. Each run preserves screenshots, state snapshots, logs and process/container identities under ignored `tests/runs/`.

The native probe at `tests/tools/account-native-probe.c` checks URL restrictions, JSON/Unicode parsing, malformed callback parameters and preservation of rotated credentials during shutdown. It uses and removes a uniquely named QA vault entry.

Explicit launcher-only QA hooks: `LARECOMP_ACCOUNT_QA_DIR`, `LARECOMP_ACCOUNT_QA_NO_BROWSER`, `LARECOMP_ACCOUNT_QA_TIMEOUT_MS` and `LARECOMP_ACCOUNT_STORAGE_SCOPE`. The action file supports account/profile/inbox jobs, preview/claim/resume/clear jobs, captures and quit. Code-entry QA uses a known public fixture code, not a private production code. Snapshots contain public UI state; the captured authorization URL contains state and the public PKCE challenge, never the verifier, authorization code, password or session credentials. These hooks are inactive without their environment overrides. The protocol test drives Keycloak HTTP forms instead of opening the tester's browser; browser opening in normal use uses the existing SDL URL-opening API.
