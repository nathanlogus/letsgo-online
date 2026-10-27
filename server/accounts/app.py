"""HTTP account and gift entitlement API. All rewards remain server-side entitlements."""
from contextlib import asynccontextmanager, contextmanager
from datetime import datetime, timezone
import hashlib
import json
import re
import secrets
import time
from uuid import UUID, uuid4

from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.encoders import jsonable_encoder
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse
from pathlib import Path
import httpx
from psycopg import OperationalError
from psycopg.errors import LockNotAvailable
from psycopg.types.json import Jsonb
from psycopg_pool import PoolTimeout
from starlette.concurrency import run_in_threadpool

from .auth import IdentityVerifier, authenticate, create_session, digest, fail, issue_pair, now
from .config import Config
from .db import Database
from .models import (AccountAction, AckInput, CampaignAction, CampaignInput, CardInput, ClaimInput,
                     CodeInput, NATIVE_ADAPTER, NativeCompleteInput, ProfileInput, RefreshInput, TokenInput)


def canonical(value):
    return json.dumps(jsonable_encoder(value), sort_keys=True, separators=(",", ":"))


def reward_hash(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def normalize_code(value):
    code = value.strip().replace("-", "").upper()
    if not re.fullmatch(r"[A-Z0-9]{6,64}", code, flags=re.ASCII):
        fail(422, "invalid_code_format")
    return code


def audit(conn, actor, action, target, request_id):
    conn.execute("INSERT INTO audit_events(actor_id,action,target_id,request_id) VALUES(%s,%s,%s,%s)",
                 (actor, action, target, request_id))


def claim_view(row):
    return jsonable_encoder({k: row[k] for k in ("id", "campaign_id", "profile_id", "adapter", "reward",
                             "reward_digest", "state", "created_at", "acknowledged_at", "card_id")})


def item_count(reward):
    return sum(i["quantity"] for i in reward["items"]) if reward["kind"] == "item_bundle" else 0


def contents(reward, native):
    """What the launcher shows: item count, or the Pokemon's species and level; native = a Mystery Gift card."""
    pokemon = reward.get("pokemon") or {}
    return {"item_count": item_count(reward), "species": pokemon.get("species", 0), "level": pokemon.get("level", 0),
            "native": native}


class BoundedRequests:
    """Bound actual body bytes including chunked input; fail before parsing sensitive JSON."""
    def __init__(self, app, limit=32768):
        self.app, self.limit = app, limit

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        parts, total = [], 0
        while True:
            event = await receive()
            if event["type"] == "http.disconnect":
                return
            body = event.get("body", b"")
            total += len(body)
            if total > self.limit:
                response = JSONResponse({"error": {"code": "request_too_large"}}, status_code=413,
                                        headers={"Cache-Control": "no-store"})
                return await response(scope, receive, send)
            parts.append(body)
            if not event.get("more_body", False):
                break
        delivered = False

        async def replay():
            nonlocal delivered
            if not delivered:
                delivered = True
                return {"type": "http.request", "body": b"".join(parts), "more_body": False}
            return await receive()
        await self.app(scope, replay, send)


def create_app(config=None, verifier=None):
    config = config or Config.from_env()
    db = Database(config.database_url)
    identity = verifier or IdentityVerifier(config)

    @asynccontextmanager
    async def lifespan(app):
        try:
            await run_in_threadpool(db.open)
            yield
        finally:
            await run_in_threadpool(db.close)

    app = FastAPI(title="Let's Go account and Mystery Gift API", version="0.1.0", lifespan=lifespan,
                  docs_url="/docs" if config.environment != "production" else None,
                  redoc_url=None)
    app.add_middleware(BoundedRequests)
    app.state.db, app.state.config = db, config

    def check_rate(ip, group):
        minute = int(time.time()) // 60
        limit = max(1, config.rate_limit // 6) if group == "auth" else config.rate_limit
        key = digest(config, "rate", ip + "\0" + group)
        with db.pool.connection() as conn:
            count = conn.execute("""INSERT INTO rate_buckets(digest,minute,count) VALUES(%s,%s,1)
                ON CONFLICT(digest,minute) DO UPDATE SET count=rate_buckets.count+1 RETURNING count""",
                                 (key, minute)).fetchone()["count"]
        return count <= limit

    @app.middleware("http")
    async def security(request: Request, call_next):
        request.state.request_id = str(uuid4())
        if config.environment == "production" and request.url.scheme != "https" and request.url.path not in ("/health/live", "/health/ready"):
            response = JSONResponse({"error": {"code": "https_required"}}, status_code=400)
        else:
            try:
                if request.url.path.startswith("/v1/"):
                    group = "auth" if request.url.path.startswith("/v1/auth/") else "api"
                    ip = request.client.host if request.client else "unknown"
                    allowed = await run_in_threadpool(check_rate, ip, group)
                    if not allowed:
                        response = JSONResponse({"error": {"code": "rate_limited"}}, status_code=429,
                                                headers={"Retry-After": str(60-int(time.time())%60)})
                    else:
                        response = await call_next(request)
                else:
                    response = await call_next(request)
            except (OperationalError, PoolTimeout):
                response = JSONResponse({"error": {"code": "database_unavailable"}}, status_code=503)
        response.headers.setdefault("Cache-Control", "no-store")  # (the dashboard artwork may be cached)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Request-ID"] = request.state.request_id
        if config.environment == "production":
            response.headers["Strict-Transport-Security"] = "max-age=31536000"
        return response

    @app.exception_handler(HTTPException)
    async def http_error(request, exc):
        return JSONResponse({"error": {"code": exc.detail}, "request_id": request.state.request_id},
                            status_code=exc.status_code, headers=exc.headers)

    @app.exception_handler(RequestValidationError)
    async def invalid_input(request, exc):
        # Pydantic's default errors echo inputs, which may contain credentials or codes.
        return JSONResponse({"error": {"code": "invalid_request", "fields": [
            {"location": list(e["loc"]), "type": e["type"]} for e in exc.errors()]},
            "request_id": request.state.request_id}, status_code=422)

    @contextmanager
    def principal(authorization, admin=False):
        if not authorization.startswith("Bearer "):
            fail(401, "authentication_required")
        with db.pool.connection() as conn:
            account = authenticate(conn, config, authorization[7:])
            if admin:
                if account["role"] != "admin":
                    fail(403, "admin_required")
                if config.admin_require_mfa and (not account["mfa_at"] or (now()-account["mfa_at"]).total_seconds() > config.admin_mfa_seconds):
                    fail(403, "recent_mfa_required")
            yield conn, account

    @app.get("/health/live")
    def live():
        return {"status": "ok"}

    @app.get("/health/ready")
    def ready():
        with db.pool.connection() as conn:
            conn.execute("SELECT 1")
        return {"status": "ok", "save_delivery_enabled": False}

    @app.get("/v1/auth/config")
    def auth_config():
        return {"issuer": config.oidc_issuer, "audience": config.oidc_audience,
                "native_flow": "authorization_code_pkce_s256", "exchange_token": "API access JWT",
                "save_delivery_enabled": False}

    @app.post("/v1/auth/exchange")
    def exchange(body: TokenInput, request: Request):
        claims = identity.verify(body.token)
        return login_session(claims, request)

    @app.get("/v1/auth/native/config")
    def native_config():
        metadata = identity.provider()
        return {"issuer": config.oidc_issuer, "authorization_endpoint": metadata["authorization_endpoint"],
                "client_id": config.native_client_id, "redirect_uri": config.native_redirect_uri}

    @app.post("/v1/auth/native/complete")
    def native_complete(body: NativeCompleteInput, request: Request):
        return login_session(identity.complete_native(body.code, body.code_verifier, body.nonce), request)

    # The dashboard (/admin/): the same code + PKCE exchange with its own public client and redirect. The admin API
    # still requires the admin role and a recent MFA sign-in (amr "mfa", auth_time).
    @app.get("/v1/auth/admin/config")
    def admin_config():
        metadata = identity.provider()
        return {"issuer": config.oidc_issuer, "authorization_endpoint": metadata["authorization_endpoint"],
                "client_id": config.admin_client_id, "redirect_uri": config.admin_redirect_uri}

    @app.post("/v1/auth/admin/complete")
    def admin_complete(body: NativeCompleteInput, request: Request):
        return login_session(identity.complete_native(body.code, body.code_verifier, body.nonce,
                                                      config.admin_client_id, config.admin_redirect_uri), request)

    def login_session(claims, request):
        display = str(claims.get("preferred_username", "Trainer"))
        display = "".join(c for c in display if c.isprintable()).strip()[:64] or "Trainer"
        mfa_at = None
        auth_time = claims.get("auth_time")
        if isinstance(claims.get("amr"), list) and "mfa" in claims["amr"] and type(auth_time) is int:
            if 0 <= time.time()-auth_time <= config.admin_mfa_seconds:
                mfa_at = datetime.fromtimestamp(auth_time, timezone.utc)
        with db.pool.connection() as conn:
            account = conn.execute("""INSERT INTO accounts(id,issuer,subject,display_name) VALUES(%s,%s,%s,%s)
                ON CONFLICT(issuer,subject) DO UPDATE SET display_name=EXCLUDED.display_name RETURNING *""",
                (uuid4(), config.oidc_issuer, claims["sub"], display)).fetchone()
            if account["status"] != "active":
                fail(403, "account_disabled")
            tokens = create_session(conn, config, account["id"], mfa_at)
            audit(conn, account["id"], "session.created", None, request.state.request_id)
        return tokens

    @app.post("/v1/auth/refresh")
    def refresh(body: RefreshInput, request: Request):
        replay = False
        with db.pool.connection() as conn:
            token_digest = digest(config, "refresh", body.refresh_token)
            first = conn.execute("""SELECT s.account_id FROM credentials c JOIN sessions s ON s.id=c.session_id
                WHERE c.digest=%s AND c.kind='refresh'""", (token_digest,)).fetchone()
            if not first:
                fail(401, "invalid_refresh_token")
            account = conn.execute("SELECT * FROM accounts WHERE id=%s FOR UPDATE", (first["account_id"],)).fetchone()
            row = conn.execute("""SELECT c.used,c.expires_at AS credential_expiry,s.* FROM credentials c
                JOIN sessions s ON s.id=c.session_id WHERE c.digest=%s FOR UPDATE OF c,s""", (token_digest,)).fetchone()
            if row["used"]:
                conn.execute("UPDATE sessions SET revoked=true WHERE id=%s", (row["id"],))
                audit(conn, account["id"], "session.refresh_replay", row["id"], request.state.request_id)
                replay = True  # Commit revocation before returning the error.
            elif row["revoked"] or account["status"] != "active" or min(row["expires_at"], row["credential_expiry"]) <= now():
                fail(401, "invalid_refresh_token")
            else:
                conn.execute("UPDATE credentials SET used=true WHERE digest=%s", (token_digest,))
                conn.execute("DELETE FROM credentials WHERE session_id=%s AND kind='access'", (row["id"],))
                tokens = issue_pair(conn, config, row["id"], row["expires_at"])
        if replay:
            fail(401, "refresh_token_reused_session_revoked")
        return tokens

    @app.post("/v1/auth/logout", status_code=204)
    def logout(request: Request, authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            conn.execute("UPDATE sessions SET revoked=true WHERE id=%s", (account["session_id"],))
            audit(conn, account["id"], "session.logout", account["session_id"], request.state.request_id)

    @app.post("/v1/auth/logout-all", status_code=204)
    def logout_all(request: Request, authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            conn.execute("UPDATE sessions SET revoked=true WHERE account_id=%s", (account["id"],))
            audit(conn, account["id"], "session.logout_all", None, request.state.request_id)

    @app.get("/v1/me")
    def me(authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            return {k: account[k] for k in ("id", "display_name", "role", "status")}

    @app.post("/v1/profiles", status_code=201)
    def profile(body: ProfileInput, request: Request, authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            count = conn.execute("SELECT count(*) AS n FROM profiles WHERE account_id=%s", (account["id"],)).fetchone()["n"]
            existing = conn.execute("SELECT * FROM profiles WHERE id=%s", (body.id,)).fetchone()
            if existing:
                if existing["account_id"] != account["id"] or existing["edition"] != body.edition or existing["label"] != body.label:
                    fail(409, "profile_conflict")
                return {k: existing[k] for k in ("id", "edition", "label")}
            if count >= 20:
                fail(409, "profile_limit")
            inserted = conn.execute("""INSERT INTO profiles(id,account_id,edition,label) VALUES(%s,%s,%s,%s)
                ON CONFLICT(id) DO NOTHING RETURNING id,edition,label""", (body.id, account["id"], body.edition, body.label)).fetchone()
            if not inserted:
                fail(409, "profile_conflict")
            audit(conn, account["id"], "profile.bound", body.id, request.state.request_id)
            return inserted

    @app.get("/v1/profiles")
    def profiles(authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            return conn.execute("SELECT id,edition,label FROM profiles WHERE account_id=%s ORDER BY created_at,id", (account["id"],)).fetchall()

    @app.get("/v1/profiles/{profile_id}")
    def get_profile(profile_id: UUID, authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            row = conn.execute("SELECT id,edition,label FROM profiles WHERE id=%s AND account_id=%s", (profile_id, account["id"])).fetchone()
            if not row:
                fail(404, "profile_not_found")
            return row

    @app.get("/v1/inbox")
    def inbox(authorization: str = Header(default=""), kind: str = "available", limit: int = 6,
              offset: int = 0, profile_id: UUID | None = None):
        if kind not in ("available", "pending", "received") or not 1 <= limit <= 50 or not 0 <= offset <= 100000:
            fail(422, "invalid_inbox_query")
        with principal(authorization) as (conn, account):
            profile = None
            if profile_id:
                profile = conn.execute("SELECT * FROM profiles WHERE id=%s AND account_id=%s", (profile_id, account["id"])).fetchone()
                if not profile:
                    fail(404, "profile_not_found")
            if kind == "available":
                rows = conn.execute("""SELECT c.id,c.title,c.reward,c.requires_code,c.adapters ? %s AS native FROM campaigns c
                    WHERE c.state='published' AND c.starts_at<=now() AND c.ends_at>now()
                    AND (c.stock IS NULL OR c.allocated<c.stock)
                    AND (%s::text IS NULL OR c.editions ? %s::text)
                    AND NOT EXISTS(SELECT 1 FROM claims cl WHERE cl.campaign_id=c.id AND cl.account_id=%s)
                    ORDER BY c.created_at,c.id LIMIT %s OFFSET %s""",
                    (NATIVE_ADAPTER, profile["edition"] if profile else None, profile["edition"] if profile else None,
                     account["id"], limit+1, offset)).fetchall()
            else:
                rows = conn.execute("""SELECT cl.id,c.title,cl.reward,p.label AS profile_label,cl.adapter=%s AS native FROM claims cl
                    JOIN campaigns c ON c.id=cl.campaign_id JOIN profiles p ON p.id=cl.profile_id
                    WHERE cl.account_id=%s AND cl.state=%s AND (%s::uuid IS NULL OR cl.profile_id=%s::uuid)
                    ORDER BY cl.created_at,cl.id LIMIT %s OFFSET %s""",
                    (NATIVE_ADAPTER, account["id"], "allocated" if kind == "pending" else "client_reported_delivered",
                     profile_id, profile_id, limit+1, offset)).fetchall()
            return {"items": [{"id": r["id"], "title": r["title"], "status": kind, **contents(r["reward"], r["native"]),
                               "requires_code": r.get("requires_code", False), "profile_label": r.get("profile_label", "")}
                              for r in rows[:limit]], "has_more": len(rows)>limit, "offset": offset}

    @app.get("/v1/gifts")
    def gifts(authorization: str = Header(default=""), limit: int = 50, offset: int = 0):
        if not 1 <= limit <= 100 or not 0 <= offset <= 100000:
            fail(422, "invalid_pagination")
        with principal(authorization) as (conn, account):
            return conn.execute("""SELECT c.id,c.title,c.starts_at,c.ends_at,c.editions,c.adapters,c.reward,
                c.requires_code,c.stock,c.allocated,cl.state AS claim_state,cl.id AS claim_id
                FROM campaigns c LEFT JOIN claims cl ON cl.campaign_id=c.id AND cl.account_id=%s
                WHERE c.state='published' AND c.starts_at<=now() AND c.ends_at>now()
                ORDER BY c.created_at,c.id LIMIT %s OFFSET %s""", (account["id"], limit, offset)).fetchall()

    @app.post("/v1/claims", status_code=201)
    def claim(body: ClaimInput, request: Request, authorization: str = Header(default=""),
              idempotency_key: str = Header(default="")):
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{8,128}", idempotency_key):
            fail(422, "idempotency_key_required")
        normalized = normalize_code(body.code) if body.code is not None else None
        request_data = body.model_dump(mode="json")
        request_data["code"] = digest(config, "code", normalized) if normalized else None
        req_digest = digest(config, "request", canonical(request_data))
        with principal(authorization) as (conn, account):
            # Account lock from authenticate serializes request keys across workers/restarts.
            old = conn.execute("SELECT * FROM request_keys WHERE account_id=%s AND operation='claim' AND key=%s",
                               (account["id"], idempotency_key)).fetchone()
            if old:
                if old["request_digest"] != req_digest:
                    fail(409, "idempotency_key_conflict")
                return old["response"]
            profile = conn.execute("SELECT * FROM profiles WHERE id=%s AND account_id=%s", (body.profile_id, account["id"])).fetchone()
            if not profile:
                fail(404, "profile_not_found")
            code = None
            campaign_id = body.campaign_id
            if normalized:
                code = conn.execute("SELECT * FROM gift_codes WHERE digest=%s", (digest(config, "code", normalized),)).fetchone()
                if not code:
                    fail(404, "gift_unavailable")
                campaign_id = code["campaign_id"]
            campaign = conn.execute("SELECT * FROM campaigns WHERE id=%s FOR UPDATE", (campaign_id,)).fetchone()
            if not campaign or campaign["state"] != "published" or not campaign["starts_at"] <= now() < campaign["ends_at"]:
                fail(409, "gift_unavailable")
            if campaign["requires_code"] and not code:
                fail(409, "code_required")
            if profile["edition"] not in campaign["editions"] or body.adapter not in campaign["adapters"]:
                fail(409, "incompatible_profile_or_adapter")
            if conn.execute("SELECT 1 FROM claims WHERE account_id=%s AND campaign_id=%s", (account["id"], campaign_id)).fetchone():
                fail(409, "already_claimed")
            if campaign["stock"] is not None and campaign["allocated"] >= campaign["stock"]:
                fail(409, "out_of_stock")
            if code:
                # Every consumer locks the campaign before its code row.
                code = conn.execute("SELECT * FROM gift_codes WHERE id=%s FOR UPDATE", (code["id"],)).fetchone()
                if code["uses"] >= code["max_uses"]:
                    fail(409, "code_exhausted")
                conn.execute("UPDATE gift_codes SET uses=uses+1 WHERE id=%s", (code["id"],))
            card_id = None
            if body.adapter == NATIVE_ADAPTER:
                # The profile's next card id (one save's received flags: never reused); the row lock orders claims.
                card = conn.execute("""UPDATE profiles SET next_card_id=next_card_id+1 WHERE id=%s AND next_card_id<2000
                    RETURNING next_card_id-1 AS card_id""", (body.profile_id,)).fetchone()
                if not card:
                    fail(409, "profile_card_ids_exhausted")
                card_id = card["card_id"]
            conn.execute("UPDATE campaigns SET allocated=allocated+1 WHERE id=%s", (campaign_id,))
            row = conn.execute("""INSERT INTO claims(id,account_id,campaign_id,profile_id,code_id,adapter,reward,reward_digest,card_id)
                VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s) RETURNING *""",
                (uuid4(), account["id"], campaign_id, body.profile_id, code["id"] if code else None,
                 body.adapter, Jsonb(campaign["reward"]), campaign["reward_digest"], card_id)).fetchone()
            result = claim_view(row)
            conn.execute("INSERT INTO request_keys VALUES(%s,'claim',%s,%s,%s)",
                         (account["id"], idempotency_key, req_digest, Jsonb(result)))
            audit(conn, account["id"], "gift.allocated", row["id"], request.state.request_id)
            return result

    @app.post("/v1/gift-preview")
    def gift_preview(body: ClaimInput, authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            profile = conn.execute("SELECT * FROM profiles WHERE id=%s AND account_id=%s", (body.profile_id, account["id"])).fetchone()
            if not profile:
                fail(404, "profile_not_found")
            code = None
            campaign_id = body.campaign_id
            if body.code is not None:
                code = conn.execute("SELECT * FROM gift_codes WHERE digest=%s", (digest(config, "code", normalize_code(body.code)),)).fetchone()
                if not code:
                    fail(404, "gift_unavailable")
                campaign_id = code["campaign_id"]
            c = conn.execute("SELECT * FROM campaigns WHERE id=%s", (campaign_id,)).fetchone()
            if not c or c["published_at"] is None:
                fail(404, "gift_unavailable")
            existing = conn.execute("SELECT id,profile_id FROM claims WHERE account_id=%s AND campaign_id=%s", (account["id"], campaign_id)).fetchone()
            reason = ""
            if existing:
                reason = "already_claimed"
            elif c["state"] != "published" or not c["starts_at"] <= now() < c["ends_at"]:
                reason = "gift_unavailable"
            elif profile["edition"] not in c["editions"] or body.adapter not in c["adapters"]:
                reason = "incompatible_profile_or_adapter"
            elif c["requires_code"] and not code:
                reason = "code_required"
            elif c["stock"] is not None and c["allocated"] >= c["stock"]:
                reason = "out_of_stock"
            elif code and code["uses"] >= code["max_uses"]:
                reason = "code_exhausted"
            return {"campaign_id": c["id"], "profile_id": profile["id"], "profile_label": profile["label"],
                    "title": c["title"], "reward_digest": c["reward_digest"], "reward": c["reward"],
                    **contents(c["reward"], NATIVE_ADAPTER in c["adapters"]), "adapters": c["adapters"], "eligible": not reason,
                    "reason": reason, "existing_claim_id": existing["id"] if existing else None}

    @app.get("/v1/claims")
    def claims(authorization: str = Header(default=""), limit: int = 50, offset: int = 0):
        if not 1 <= limit <= 100 or not 0 <= offset <= 100000:
            fail(422, "invalid_pagination")
        with principal(authorization) as (conn, account):
            return [claim_view(r) for r in conn.execute("SELECT * FROM claims WHERE account_id=%s ORDER BY created_at,id LIMIT %s OFFSET %s",
                                                       (account["id"], limit, offset))]

    @app.get("/v1/claims/{claim_id}")
    def get_claim(claim_id: UUID, authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            row = conn.execute("SELECT * FROM claims WHERE id=%s AND account_id=%s", (claim_id, account["id"])).fetchone()
            if not row:
                fail(404, "claim_not_found")
            return claim_view(row)

    @app.post("/v1/claims/{claim_id}/ack")
    def ack(claim_id: UUID, body: AckInput, request: Request, authorization: str = Header(default="")):
        with principal(authorization) as (conn, account):
            row = conn.execute("SELECT * FROM claims WHERE id=%s AND account_id=%s FOR UPDATE", (claim_id, account["id"])).fetchone()
            if not row:
                fail(404, "claim_not_found")
            if row["profile_id"] != body.profile_id or row["reward_digest"] != body.reward_digest:
                fail(409, "delivery_receipt_mismatch")
            if row["receipt_digest"] and row["receipt_digest"] != body.receipt_digest:
                fail(409, "delivery_receipt_mismatch")
            if row["state"] == "allocated":
                row = conn.execute("""UPDATE claims SET state='client_reported_delivered',receipt_digest=%s,
                    acknowledged_at=now() WHERE id=%s RETURNING *""", (body.receipt_digest, claim_id)).fetchone()
                audit(conn, account["id"], "gift.client_acknowledged", claim_id, request.state.request_id)
            return claim_view(row)

    @app.post("/v1/claims/{claim_id}/card")
    def reassign_card(claim_id: UUID, body: CardInput, request: Request, authorization: str = Header(default="")):
        """The save already has this card id's received flag (an official distribution, or a save from elsewhere): the
        claim gets the profile's next id. Only while card_id is still the reported one, so a retry can't burn ids."""
        with principal(authorization) as (conn, account):
            row = conn.execute("SELECT * FROM claims WHERE id=%s AND account_id=%s FOR UPDATE", (claim_id, account["id"])).fetchone()
            if not row:
                fail(404, "claim_not_found")
            if row["profile_id"] != body.profile_id or row["adapter"] != NATIVE_ADAPTER or row["state"] != "allocated":
                fail(409, "card_not_reassignable")
            if row["card_id"] == body.card_id:
                card = conn.execute("""UPDATE profiles SET next_card_id=next_card_id+1 WHERE id=%s AND next_card_id<2000
                    RETURNING next_card_id-1 AS card_id""", (row["profile_id"],)).fetchone()
                if not card:
                    fail(409, "profile_card_ids_exhausted")
                row = conn.execute("UPDATE claims SET card_id=%s WHERE id=%s RETURNING *", (card["card_id"], claim_id)).fetchone()
                audit(conn, account["id"], "gift.card_reassigned", claim_id, request.state.request_id)
            return claim_view(row)

    @app.get("/v1/admin/catalog")
    def admin_catalog(authorization: str = Header(default="")):
        """The dashboard's pickers: giftable items (with their bag pocket), Let's Go Pokemon and moves (migration 003)."""
        with principal(authorization, admin=True) as (conn, account):
            return {"items": conn.execute("SELECT id,name,pocket FROM catalog_items ORDER BY pocket,name").fetchall(),
                    "species": conn.execute("SELECT id,name,alolan FROM catalog_species ORDER BY id").fetchall(),
                    "moves": conn.execute("SELECT id,name FROM catalog_moves ORDER BY name").fetchall()}

    @app.post("/v1/admin/campaigns", status_code=201)
    def create_campaign(body: CampaignInput, request: Request, authorization: str = Header(default="")):
        reward = body.reward.model_dump(mode="json")
        with principal(authorization, admin=True) as (conn, account):
            if body.reward.kind == "pokemon":
                moves = body.reward.pokemon.moves
                known = conn.execute("SELECT count(*) AS n FROM catalog_moves WHERE id=ANY(%s)", (moves,)).fetchone()["n"]
                if known != len(moves):
                    fail(422, "move_not_in_lets_go")
            row = conn.execute("""INSERT INTO campaigns(id,title,starts_at,ends_at,requires_code,editions,adapters,reward,reward_digest,stock)
                VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s) RETURNING *""",
                (uuid4(), body.title, body.starts_at, body.ends_at, body.requires_code, Jsonb(body.editions),
                 Jsonb(body.adapters), Jsonb(reward), reward_hash(reward), body.stock)).fetchone()
            audit(conn, account["id"], "campaign.created", row["id"], request.state.request_id)
            return row

    @app.get("/v1/admin/campaigns")
    def admin_campaigns(authorization: str = Header(default=""), limit: int = 50, offset: int = 0):
        if not 1 <= limit <= 100 or not 0 <= offset <= 100000:
            fail(422, "invalid_pagination")
        with principal(authorization, admin=True) as (conn, account):
            return conn.execute("SELECT * FROM campaigns ORDER BY created_at,id LIMIT %s OFFSET %s", (limit, offset)).fetchall()

    @app.post("/v1/admin/campaigns/{campaign_id}/action")
    def campaign_action(campaign_id: UUID, body: CampaignAction, request: Request, authorization: str = Header(default="")):
        with principal(authorization, admin=True) as (conn, account):
            row = conn.execute("SELECT * FROM campaigns WHERE id=%s FOR UPDATE", (campaign_id,)).fetchone()
            if not row:
                fail(404, "campaign_not_found")
            state = "published" if body.action == "publish" else "paused"
            row = conn.execute("UPDATE campaigns SET state=%s,published_at=CASE WHEN %s='published' THEN coalesce(published_at,now()) ELSE published_at END WHERE id=%s RETURNING *",
                               (state, state, campaign_id)).fetchone()
            audit(conn, account["id"], "campaign."+body.action, campaign_id, request.state.request_id)
            return row

    @app.post("/v1/admin/campaigns/{campaign_id}/codes", status_code=201)
    def create_code(campaign_id: UUID, body: CodeInput, request: Request, authorization: str = Header(default="")):
        value = normalize_code(body.code) if body.code else secrets.token_hex(16).upper()
        with principal(authorization, admin=True) as (conn, account):
            if not conn.execute("SELECT 1 FROM campaigns WHERE id=%s FOR UPDATE", (campaign_id,)).fetchone():
                fail(404, "campaign_not_found")
            row = conn.execute("""INSERT INTO gift_codes(id,campaign_id,digest,max_uses) VALUES(%s,%s,%s,%s)
                ON CONFLICT(digest) DO NOTHING RETURNING id,max_uses""",
                (uuid4(), campaign_id, digest(config, "code", value), body.max_uses)).fetchone()
            if not row:
                fail(409, "code_conflict")
            audit(conn, account["id"], "code.created", row["id"], request.state.request_id)
            return {**row, "code": value}  # Shown once, never persisted in plaintext.

    @app.get("/v1/admin/campaigns/{campaign_id}/claims")
    def inspect_claims(campaign_id: UUID, authorization: str = Header(default=""), limit: int = 50, offset: int = 0):
        if not 1 <= limit <= 100 or not 0 <= offset <= 100000:
            fail(422, "invalid_pagination")
        with principal(authorization, admin=True) as (conn, account):
            return [dict(claim_view(r), account_id=str(r["account_id"])) for r in conn.execute(
                "SELECT * FROM claims WHERE campaign_id=%s ORDER BY created_at,id LIMIT %s OFFSET %s", (campaign_id, limit, offset))]

    @app.get("/v1/admin/accounts")
    def admin_accounts(authorization: str = Header(default=""), query: str = "", limit: int = 50, offset: int = 0):
        if not 1 <= limit <= 100 or not 0 <= offset <= 100000 or len(query) > 64:
            fail(422, "invalid_pagination")
        with principal(authorization, admin=True) as (conn, account):
            return conn.execute("""SELECT a.id,a.display_name,a.role,a.status,a.created_at,
                (SELECT count(*) FROM profiles p WHERE p.account_id=a.id) AS profiles,
                (SELECT count(*) FROM claims c WHERE c.account_id=a.id) AS claims
                FROM accounts a WHERE %s='' OR a.display_name ILIKE %s OR a.id::text=%s
                ORDER BY a.created_at DESC,a.id LIMIT %s OFFSET %s""",
                (query, "%" + query.replace("%", r"\%").replace("_", r"\_") + "%", query, limit, offset)).fetchall()

    @app.get("/v1/admin/audit")
    def admin_audit(authorization: str = Header(default=""), limit: int = 50, offset: int = 0):
        if not 1 <= limit <= 100 or not 0 <= offset <= 100000:
            fail(422, "invalid_pagination")
        with principal(authorization, admin=True) as (conn, account):
            return conn.execute("""SELECT e.*,a.display_name AS actor_name FROM audit_events e LEFT JOIN accounts a ON a.id=e.actor_id
                ORDER BY e.created_at DESC LIMIT %s OFFSET %s""", (limit, offset)).fetchall()

    def relay_call(method, path):
        if not config.relay_admin_url:
            fail(404, "relay_not_configured")
        try:
            r = httpx.request(method, config.relay_admin_url.rstrip("/") + path, timeout=3, trust_env=False,
                              headers={"Authorization": "Bearer " + config.relay_admin_token})
        except httpx.HTTPError:
            fail(503, "relay_unavailable")
        if r.status_code == 404:
            fail(404, "relay_room_not_found")
        if r.status_code != 200:
            fail(502, "relay_error")
        return r.json()

    @app.get("/v1/admin/relay")
    def admin_relay(authorization: str = Header(default="")):
        with principal(authorization, admin=True):
            pass  # (the relay call doesn't hold the database connection)
        return relay_call("GET", "/rooms")

    @app.post("/v1/admin/relay/rooms/{room}/close")
    def admin_relay_close(room: str, request: Request, authorization: str = Header(default="")):
        if not re.fullmatch(r"[0-9a-f]{12}", room):
            fail(422, "invalid_room")
        with principal(authorization, admin=True) as (conn, account):
            audit(conn, account["id"], "relay.room_closed", None, request.state.request_id)
        return relay_call("POST", f"/rooms/{room}/close")

    dashboard = Path(__file__).parent / "dashboard"

    @app.get("/admin/", include_in_schema=False)
    def admin_page():
        return FileResponse(dashboard / "index.html", headers={"Content-Security-Policy":
            "default-src 'none'; script-src 'self'; style-src 'self' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; "
            "connect-src 'self'; img-src 'self' data:; "
            "base-uri 'none'; form-action 'none'; frame-ancestors 'none'", "Referrer-Policy": "no-referrer"})

    @app.get("/admin/{name}", include_in_schema=False)
    def admin_asset(name: str):
        if name not in ("app.js", "style.css", "icon.svg"):  # (icon.svg: a copy of docs/icon/icon.svg, the project's icon)
            fail(404, "not_found")
        return FileResponse(dashboard / name)

    @app.get("/admin/art/{name}", include_in_schema=False)
    def admin_art(name: str):
        # (the artwork folder is optional: without it the page keeps its plain colours)
        path = Path(config.dashboard_assets) / name if config.dashboard_assets else None
        if not path or not re.fullmatch(r"[a-z0-9-]{1,32}\.(png|jpg)", name) or not path.is_file():
            fail(404, "not_found")
        return FileResponse(path, media_type="image/png" if name.endswith(".png") else "image/jpeg", headers={"Cache-Control": "max-age=86400"})

    @app.post("/v1/admin/accounts/{account_id}/status")
    def account_status(account_id: UUID, body: AccountAction, request: Request, authorization: str = Header(default="")):
        with principal(authorization, admin=True) as (conn, account):
            if account_id == account["id"]:
                fail(409, "cannot_disable_self")
            # Avoid opposite-order account locks when two admins act on each other.
            try:
                locked = conn.execute("SELECT id FROM accounts WHERE id=%s FOR UPDATE NOWAIT", (account_id,)).fetchone()
            except LockNotAvailable:
                fail(409, "account_busy_retry")
            if not locked:
                fail(404, "account_not_found")
            conn.execute("UPDATE accounts SET status=%s WHERE id=%s", (body.status, account_id))
            if body.status == "disabled":
                conn.execute("UPDATE sessions SET revoked=true WHERE account_id=%s", (account_id,))
            audit(conn, account["id"], "account."+body.status, account_id, request.state.request_id)
            return {"id": account_id, "status": body.status}

    return app
