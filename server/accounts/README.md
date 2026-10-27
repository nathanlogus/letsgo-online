# Accounts and Mystery Gift service

Python/FastAPI + PostgreSQL backend for local-profile accounts and item-gift entitlements. Runs independently of `server/coop_relay.py`. Neither this service nor its tests change game saves. OpenAPI is at `/openapi.json`, with an interactive `/docs` page in development.

Implemented: JWT identity verification, account creation keyed by issuer/subject, revocable opaque sessions, single-use rotating refresh credentials, profile ownership, staff campaign creation/publication/pause, public or generated codes, finite stock, account-wide claim uniqueness, idempotent retries, frozen reward payloads, delivery acknowledgements and audit records. Claims are persistent entitlements. Acknowledgements are explicitly **client-reported delivery**, not proof of game-save mutation.

## Existing local Keycloak

Use the Docker Keycloak instance from `C:/Users/Nathan/orca/ssrecomp/server/docker-compose.yml`. Its public issuer is `http://localhost:8080/realms/johto`. The Let’s Go application uses a separate **public** client `letsgo-native`, PKCE S256 and an access-token-only audience mapper for `letsgo-api`. Existing Keycloak users can sign in; existing Johto clients, passwords and database volumes are not modified.

The launcher Account page now opens the system browser and listens only on the exact redirect `http://127.0.0.1:7386/callback`. It verifies state and uses authorization code + PKCE S256. The API verifies the ID-token nonce, signature, audience and subject against the access token before creating a session. Password grants, implicit grants and service-account grants are disabled for this client. No client secret belongs in the game. See [launcher usage and QA](../../docs/ACCOUNTS-LAUNCHER.md).

The idempotent setup helper refuses to overwrite an existing client with different settings:

```powershell
tests/venvs/account-gift-api/Scripts/python.exe -m server.accounts.keycloak_setup --credentials-env C:/Users/Nathan/orca/ssrecomp/server/.env
```

This reads the Keycloak admin credentials in memory; it prints only public client/issuer information. It requires `requirements-dev.txt`. Changing the existing realm’s registration or MFA policy is a separate operational decision. Existing realm registration/recovery settings apply to players.

## Run locally

From the repository root, create a private environment file under ignored `tests/config/`, using `.env.example` as a template. Generate independent random values for `ACCOUNT_DB_PASSWORD` (hex, safe in the connection URL) and `ACCOUNT_DIGEST_SECRET` (at least 32 random bytes). Never reuse the other project's account secret.

Local Docker values:

```dotenv
ACCOUNT_ENV=development
ACCOUNT_PORT=7385
ACCOUNT_DB_PASSWORD=<random hex>
ACCOUNT_DIGEST_SECRET=<random secret>
ACCOUNT_OIDC_ISSUER=http://localhost:8080/realms/johto
ACCOUNT_OIDC_AUDIENCE=letsgo-api
ACCOUNT_OIDC_JWKS_URL=http://host.docker.internal:8080/realms/johto/protocol/openid-connect/certs
ACCOUNT_OIDC_ALGORITHM=RS256
ACCOUNT_OIDC_INTERNAL_URL=http://host.docker.internal:8080/realms/johto
```

The issuer must match the JWT exactly. `host.docker.internal` supplies the server-to-Keycloak routes inside Docker Desktop for JWKS, discovery and code exchange; it must not replace the token issuer or browser authorization URL. For a Python process running on the host, use `http://localhost:8080/.../certs` and omit `ACCOUNT_OIDC_INTERNAL_URL`.

```powershell
docker compose --env-file tests/config/letsgo-accounts.env -f server/accounts/compose.yml build api
docker compose --env-file tests/config/letsgo-accounts.env -f server/accounts/compose.yml up -d database
docker compose --env-file tests/config/letsgo-accounts.env -f server/accounts/compose.yml run --rm api python -m server.accounts migrate
docker compose --env-file tests/config/letsgo-accounts.env -f server/accounts/compose.yml up -d api
```

Project name is `letsgo-accounts`, API binds `127.0.0.1:7385`, and PostgreSQL is private with its own `account-data` volume. This leaves the other project's Compose stack alone. Migrations are explicit, checksum-verified and protected by a PostgreSQL advisory lock. API startup refuses an unmigrated schema. Preserve the database volume when updating.

For host development, install `requirements-dev.txt` into `tests/venvs/account-gift-api/`, set the environment variables and run:

```powershell
python -m server.accounts migrate
python -m uvicorn server.accounts.app:create_app --factory --host 127.0.0.1 --port 7385 --no-access-log --no-proxy-headers
```

## Login/session contract

The launcher uses `GET /v1/auth/native/config` to obtain public browser settings, then sends the returned code, its private `code_verifier`, and its original `nonce` to `POST /v1/auth/native/complete`. Provider metadata is cached briefly, checked against the configured issuer, and only same-origin issuer endpoints are accepted. The server exchanges the code using the configured public client and exact loopback redirect, verifies both signed tokens, validates nonce/subject/authorized party and any `at_hash`, then returns only the application's session pair. Provider credentials never go into settings or logs. API callers already holding an appropriate provider access token can still use `/v1/auth/exchange` as below.

1. The client completes browser PKCE login with Keycloak, requesting `openid profile` and a random state/nonce.
2. Send its **API access token**, not its ID token, as `{"token":"..."}` to `POST /v1/auth/exchange`. Signature, fixed algorithm, issuer, API audience, subject and expiry are verified against the configured JWKS. The API creates or resolves the account by issuer/subject.
3. Store the returned refresh token in the OS credential store; keep the access token in memory. Send `Authorization: Bearer <access_token>` for authenticated API calls.
4. `POST /v1/auth/refresh` with `{"refresh_token":"..."}` replaces both credentials. Access lifetime defaults to 15 minutes; the session has an absolute 30-day lifetime. Old access credentials stop working. Reusing any rotated refresh credential revokes the entire session, including its current access token. Concurrent refreshes must be serialized by the client. Losing a refresh response requires signing in again.
5. `POST /v1/auth/logout` revokes the current API session; `/v1/auth/logout-all` revokes all of that account’s API sessions. These do not log out other applications using Keycloak.

Keycloak account disable/logout is not an instantaneous revocation mechanism for previously issued API sessions. Use the service's account disable/session revoke operations for immediate API revocation. OIDC identity linking, account deletion and back-channel logout are not implemented.

## Profiles and gift endpoints

| Method / path | Purpose |
| --- | --- |
| `GET /v1/me` | Account ID, display name, role and status |
| `POST /v1/profiles` | Bind `{id,edition,label}`; edition is `pikachu` or `eevee` |
| `GET /v1/profiles` | Own bound profiles; maximum 20 per account |
| `GET /v1/profiles/{id}` | Verify an owned binding; other owners receive 404 |
| `GET /v1/inbox` | Read-only UI projection: available/pending/received, owned-profile filter, pagination and `has_more` |
| `GET /v1/gifts` | Active published campaigns and own claim status |
| `POST /v1/claims` | Allocate a campaign or code to an owned profile |
| `POST /v1/gift-preview` | Preview the same selector/destination without consuming stock, code uses or a claim |
| `GET /v1/claims` | Own history, including pending allocations |
| `GET /v1/claims/{id}` | Own individual allocation |
| `POST /v1/claims/{id}/ack` | Idempotent client delivery report |

List endpoints accept `limit` (1–100, default 50) and `offset` (default 0). Profile UUIDs are routing identities, not anti-cheat evidence or proof of ownership of a local save. A profile cannot be rebound to another account through this API.

The inbox specifically accepts `kind=available|pending|received`, `limit` (1–50, default 6), `offset` and optional owned `profile_id`. It returns `{items,has_more,offset}` with titles, status, aggregate item counts, code-required hints and destination labels. Available gifts exclude inactive/out-of-stock/already-claimed campaigns and filter edition when a profile is supplied; history includes the authenticated account's claims, including paused/expired campaigns. Reading it never allocates or acknowledges rewards. The native launcher now supports binding, browsing, preview and explicit claiming. In-game integration and native reward delivery remain open.

`POST /v1/gift-preview` uses `ClaimInput` (exactly one campaign ID or code, profile ID and adapter). It checks ownership and returns title/reward/digest, destination label, eligibility/reason and any existing owned claim ID. It does not lock/reserve stock or consume a code. Claim allocation revalidates eligibility transactionally. The launcher [persists an encrypted retry before confirmation POST](../../docs/MYSTERY-GIFT-CLAIMS.md), then reuses its key/body after an uncertain reply or restart. Clearing that local retry never resets server history.

Claim request:

```http
POST /v1/claims
Authorization: Bearer <access token>
Idempotency-Key: <stable random request UUID>
Content-Type: application/json

{"campaign_id":"<UUID>","profile_id":"<UUID>","adapter":"lgpe-items-v1"}
```

Supply `code` instead of `campaign_id` for code redemption. Codes are ASCII letters/digits, case insensitive, with hyphens removed and surrounding whitespace trimmed. Private codes are stored as domain-separated HMAC digests. Generated single-use codes contain 128 random bits and are returned only on creation.

Claims atomically check campaign window, publication, profile edition, adapter, account uniqueness, stock and code use count. They retain an immutable reward and its SHA-256 digest. The account/campaign database constraint applies across all profiles and request keys. The same key and request return the original recorded response, even after acknowledgement; a changed request with that key returns `409 idempotency_key_conflict`. A new key for an already claimed campaign returns `409 already_claimed`. Failed transactions consume no stock/code use and retain no request key.

The acknowledgement body contains `profile_id`, `reward_digest`, and a 64-character lowercase hex `receipt_digest` identifying the client’s durable receipt. Repeating it returns the same state; conflicting receipts return 409. Lack of acknowledgement never automatically restocks or cancels an allocation. Claims remain resumable after campaign pause/expiry.

## Staff operations

Sign in normally first, then grant that exact API account ID the staff role using the operator CLI inside the API container:

```powershell
docker compose --env-file tests/config/letsgo-accounts.env -f server/accounts/compose.yml exec api python -m server.accounts grant-admin <account-uuid>
```

Role assignment alone does not bypass MFA. Every staff HTTP operation requires a verified provider token with `amr` containing `mfa` and `auth_time` within 15 minutes. Refresh does not extend this MFA window. Configure a dedicated Keycloak staff authentication flow requiring a second factor and an **Authentication Method Reference** mapper based on actually completed authenticators. Do not add a static `mfa` claim. This implementation does not change the shared realm's MFA flow or grant anyone administrator access automatically; player login works without MFA. Real-provider staff MFA still needs that configuration and verification.

| Method / path | Purpose |
| --- | --- |
| `POST /v1/admin/campaigns` | Create immutable draft payload and eligibility policy |
| `GET /v1/admin/campaigns` | Preview/list campaign definitions |
| `POST /v1/admin/campaigns/{id}/action` | `{"action":"publish"}` or `{"action":"pause"}` |
| `POST /v1/admin/campaigns/{id}/codes` | Optional public `code`, `max_uses`; omit code to generate one |
| `GET /v1/admin/campaigns/{id}/claims` | Inspect allocations and destination account IDs |
| `POST /v1/admin/accounts/{id}/status` | Set `active`/`disabled`; disable revokes all API sessions |

Campaign body example:

```json
{
  "title": "Welcome gift",
  "starts_at": "2026-10-27T00:00:00Z",
  "ends_at": "2026-12-01T00:00:00Z",
  "requires_code": false,
  "editions": ["pikachu", "eevee"],
  "adapters": ["lgpe-items-v1"],
  "stock": 1000,
  "reward": {"schema_version": 1, "kind": "item_bundle", "items": [{"item_id": 4, "quantity": 5}]}
}
```

Omit `stock` for unlimited entitlements. Item definitions are bounded typed data: 1–32 unique positive 16-bit item IDs and quantities 1–999. The service does not yet verify a game-version item allowlist; a future supported game adapter must validate actual IDs, capacities and save compatibility before applying anything. `lgpe-items-v1` is a proposed contract label, not a working native delivery adapter. Pokemon gifts and arbitrary scripts are rejected by the current schema. There is no reward-edit endpoint; correcting a draft requires a new draft. Publishing/pausing preserves the payload and claim history.

Operator CLI also provides `revoke-sessions <account-uuid>` and `prune-rate-buckets`. Run rate-bucket cleanup daily. Domain-separated digest secret rotation invalidates sessions, existing code lookup and request digests; preserve it with backups and plan a migration rather than changing it in place.

## Hosting and recovery

Local HTTP mode is for loopback development only. Production requires HTTPS issuer/JWKS URLs and HTTPS requests. Put the loopback API behind a TLS reverse proxy, configure `ACCOUNT_PROXY_IPS` to that proxy’s exact source IP/network and keep port 7385 private. Never use `*` for trusted proxies. The Compose command enables forwarded headers only for the configured sources; direct Python uses none unless explicitly enabled. Health endpoints permit private HTTP health checks. Configure proxy request-header/body timeouts and do not log authorization headers, query credentials or bodies. The native relay remains unchanged and receives no new account credentials.

The API rejects bodies over 32 KiB, omits input values from validation errors, returns `Cache-Control: no-store` and generated request IDs, and uses shared PostgreSQL rate buckets (120 API requests/minute/IP, 20 auth requests/minute/IP by default). No browser CORS policy is enabled; a future web UI needs a deliberate exact-origin policy. Audit events contain actor/action/target IDs, never raw tokens, private codes or saves. Production API docs UI is disabled.

Use a dedicated least-privilege database credential for hosting, separate migration credentials as appropriate, and pin container image digests for the release environment. Take database backups plus the digest secret and identity configuration. A database backup older than issued claims can allow duplicate entitlements; reconcile before reopening allocations. The service provides no automatic reconciliation or proof of uniqueness in user-edited/restored local saves.

## Verification and remaining integration

Local automated harnesses are under git-ignored `tests/`, per repository convention:

```powershell
tests/venvs/account-gift-api/Scripts/python.exe tests/scripts/run-account-gift-checks.py --keycloak-env C:/Users/Nathan/orca/ssrecomp/server/.env
```

It creates an isolated PostgreSQL container on a random loopback port, applies migrations twice, checks authentication/MFA/session isolation, concurrent claim races, code/stock limits, rollback after an injected database failure, payload limits/redaction, and real HTTP/API restart persistence. Optional Keycloak verification uses and deletes a disposable QA user, tests the real browser authorization-code/PKCE exchange and checks the existing `johto-online` client remains unchanged. Each run preserves logs, JUnit, a database dump and exact owned process/container IDs. Only its own container/processes are cleaned up. Install the dev requirements and recreate these local harnesses if they are missing in a fresh checkout.

Not implemented: in-game account UI, native save delivery/recovery, account-aware relay transport, Pokemon distributions, identity-provider administration UI or automatic account deletion. Launcher login is implemented on Windows. In-game gift delivery is intentionally disabled (`/health/ready` reports `save_delivery_enabled: false`). See [the save transaction design](../../docs/ACCOUNTS-MYSTERY-GIFTS.md#delivery-crashes-and-the-local-save-boundary) before implementing it.
