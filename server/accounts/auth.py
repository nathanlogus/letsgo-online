from datetime import datetime, timedelta, timezone
from hashlib import sha256
import base64
import json
import hmac
import secrets
import threading
import time
from urllib.parse import urlsplit
from uuid import uuid4
import httpx
import jwt
from fastapi import HTTPException


def fail(status, code):
    raise HTTPException(status_code=status, detail=code)


def now():
    return datetime.now(timezone.utc)


def digest(config, purpose, value):
    return hmac.new(config.digest_secret.encode(), (purpose + "\0" + value).encode(), sha256).hexdigest()


class IdentityVerifier:
    def __init__(self, config):
        self.config = config
        self.keys = jwt.PyJWKClient(config.oidc_jwks_url, timeout=5) if config.oidc_jwks_url else None
        self.metadata = None
        self.metadata_until = 0
        self.metadata_lock = threading.Lock()

    def verify(self, token):
        return self.verify_jwt(token, self.config.oidc_audience)

    def verify_jwt(self, token, audience):
        if not self.keys:
            fail(503, "identity_provider_not_configured")
        try:
            key = self.keys.get_signing_key_from_jwt(token)
            claims = jwt.decode(token, key.key, algorithms=[self.config.oidc_algorithm],
                                issuer=self.config.oidc_issuer, audience=audience,
                                options={"require": ["exp", "iat", "sub", "iss", "aud"]}, leeway=5)
        except jwt.PyJWKClientConnectionError:
            fail(503, "identity_provider_unavailable")
        except (jwt.PyJWTError, ValueError, TypeError):
            fail(401, "invalid_identity_token")
        if not isinstance(claims.get("sub"), str) or not 1 <= len(claims["sub"]) <= 255:
            fail(401, "invalid_identity_token")
        return claims

    def provider(self):
        if not self.keys:
            fail(503, "identity_provider_not_configured")
        with self.metadata_lock:
            if self.metadata and time.monotonic() < self.metadata_until:
                return self.metadata
            metadata = self.provider_request("GET", self.internal_url(self.config.oidc_issuer.rstrip("/") + "/.well-known/openid-configuration"))
            if metadata.get("issuer") != self.config.oidc_issuer:
                fail(502, "invalid_identity_metadata")
            issuer = urlsplit(self.config.oidc_issuer)
            for name in ("authorization_endpoint", "token_endpoint"):
                value = metadata.get(name)
                if not isinstance(value, str):
                    fail(502, "invalid_identity_metadata")
                endpoint = urlsplit(value)
                if ((endpoint.scheme, endpoint.netloc) != (issuer.scheme, issuer.netloc) or
                    endpoint.username or endpoint.password or endpoint.fragment or endpoint.query):
                    fail(502, "invalid_identity_metadata")
            if "S256" not in metadata.get("code_challenge_methods_supported", []):
                fail(502, "pkce_not_supported")
            self.metadata, self.metadata_until = metadata, time.monotonic() + 300
            return metadata

    def internal_url(self, endpoint):
        public = self.config.oidc_issuer.rstrip('/')
        if self.config.oidc_internal_url:
            if not endpoint.startswith(public + '/'):
                fail(502, "invalid_identity_endpoint")
            return self.config.oidc_internal_url.rstrip('/') + endpoint[len(public):]
        return endpoint

    @staticmethod
    def provider_request(method, url, data=None):
        try:
            with httpx.Client(timeout=8, trust_env=False, follow_redirects=False) as client:
                with client.stream(method, url, data=data) as response:
                    if response.status_code != 200:
                        fail(401 if response.status_code in (400, 401) else 503, "identity_exchange_failed")
                    body = bytearray()
                    end = time.monotonic() + 15
                    for chunk in response.iter_bytes():
                        body.extend(chunk)
                        if len(body) > 65536 or time.monotonic() > end:
                            fail(502, "invalid_identity_response")
            result = json.loads(body)
            if not isinstance(result, dict):
                fail(502, "invalid_identity_response")
            return result
        except httpx.HTTPError:
            fail(503, "identity_provider_unavailable")
        except (ValueError, UnicodeError):
            fail(502, "invalid_identity_response")

    def complete_native(self, code, verifier, nonce, client_id=None, redirect_uri=None):
        """Authorization code + PKCE for the launcher's client, or (client_id, redirect_uri) the dashboard's."""
        client_id, redirect_uri = client_id or self.config.native_client_id, redirect_uri or self.config.native_redirect_uri
        provider = self.provider()
        tokens = self.provider_request("POST", self.internal_url(provider["token_endpoint"]), data={
            "grant_type": "authorization_code", "client_id": client_id,
            "redirect_uri": redirect_uri, "code": code, "code_verifier": verifier})
        access, identity = tokens.get("access_token"), tokens.get("id_token")
        if not isinstance(access, str) or not isinstance(identity, str) or max(len(access), len(identity)) > 32768:
            fail(401, "invalid_identity_response")
        claims = self.verify(access)
        oidc = self.verify_jwt(identity, client_id)
        if (not isinstance(oidc.get("nonce"), str) or not hmac.compare_digest(oidc["nonce"], nonce) or
            oidc["sub"] != claims["sub"] or ("azp" in oidc and oidc["azp"] != client_id)):
            fail(401, "identity_nonce_or_subject_mismatch")
        if "at_hash" in oidc:
            try:
                raw = jwt.get_algorithm_by_name(self.config.oidc_algorithm).compute_hash_digest(access.encode())
            except (ValueError, NotImplementedError):
                fail(401, "unsupported_identity_hash")
            expected = base64.urlsafe_b64encode(raw[:len(raw)//2]).decode().rstrip("=")
            if not isinstance(oidc["at_hash"], str) or not hmac.compare_digest(oidc["at_hash"], expected):
                fail(401, "identity_access_hash_mismatch")
        return claims


def issue_pair(conn, config, session_id, absolute_expiry):
    access, refresh = secrets.token_urlsafe(32), secrets.token_urlsafe(48)
    instant = now()
    expiry = min(instant + timedelta(seconds=config.access_seconds), absolute_expiry)
    for kind, token, end in (("access", access, expiry), ("refresh", refresh, absolute_expiry)):
        conn.execute("INSERT INTO credentials(digest,session_id,kind,expires_at) VALUES(%s,%s,%s,%s)",
                     (digest(config, kind, token), session_id, kind, end))
    return {"access_token": access, "refresh_token": refresh, "token_type": "Bearer",
            "expires_in": max(0, int((expiry-instant).total_seconds())),
            "refresh_expires_at": absolute_expiry.isoformat()}


def create_session(conn, config, account_id, mfa_at=None):
    sid = uuid4()
    end = now() + timedelta(seconds=config.refresh_seconds)
    conn.execute("INSERT INTO sessions(id,account_id,mfa_at,expires_at) VALUES(%s,%s,%s,%s)",
                 (sid, account_id, mfa_at, end))
    return issue_pair(conn, config, sid, end)


def authenticate(conn, config, token):
    if not token or len(token) > 512:
        fail(401, "authentication_required")
    # Lock the account for this request: role/disable changes and same-account claims serialize.
    row = conn.execute("""SELECT a.*, s.id AS session_id, s.mfa_at FROM credentials c
        JOIN sessions s ON s.id=c.session_id JOIN accounts a ON a.id=s.account_id
        WHERE c.digest=%s AND c.kind='access' AND c.expires_at>now()
          AND NOT s.revoked AND s.expires_at>now() AND a.status='active'
        FOR UPDATE OF a""", (digest(config, "access", token),)).fetchone()
    if not row:
        fail(401, "invalid_or_expired_session")
    return row
