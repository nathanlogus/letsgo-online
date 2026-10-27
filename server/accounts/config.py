from dataclasses import dataclass
import os
from urllib.parse import urlsplit


@dataclass(frozen=True)
class Config:
    database_url: str
    digest_secret: str
    environment: str = "production"
    oidc_issuer: str = ""
    oidc_audience: str = ""
    oidc_jwks_url: str = ""
    oidc_algorithm: str = "RS256"
    native_client_id: str = "letsgo-native"
    native_redirect_uri: str = "http://127.0.0.1:7386/callback"
    oidc_internal_url: str = ""
    access_seconds: int = 900
    refresh_seconds: int = 2592000
    admin_mfa_seconds: int = 900
    admin_require_mfa: bool = True  # staff need a recent two-step sign-in (amr "mfa"); off: the staff role alone
    rate_limit: int = 120
    public_url: str = ""          # this service's public base URL (the dashboard's Keycloak redirect); default http://127.0.0.1:7385
    admin_client_id: str = "letsgo-admin"  # the dashboard's public Keycloak client (keycloak_setup --client-id)
    dashboard_assets: str = ""    # the dashboard's artwork (dashboard/collect_assets.py), mounted read-only; optional
    relay_admin_url: str = ""     # the co-op relay's admin port (server/coop_relay.py), e.g. http://relay:7382
    relay_admin_token: str = ""

    def __post_init__(self):
        if self.environment not in ("production", "development", "test"):
            raise ValueError("Invalid ACCOUNT_ENV")
        if not self.database_url.startswith(("postgresql://", "postgres://")):
            raise ValueError("ACCOUNT_DATABASE_URL must use PostgreSQL")
        if len(self.digest_secret.encode()) < 32:
            raise ValueError("ACCOUNT_DIGEST_SECRET needs at least 32 bytes")
        if self.oidc_algorithm not in ("RS256", "ES256", "EdDSA"):
            raise ValueError("Unsupported OIDC algorithm")
        redirect = urlsplit(self.native_redirect_uri)
        if (redirect.scheme != "http" or redirect.hostname != "127.0.0.1" or not redirect.port or
            redirect.path != "/callback" or redirect.query or redirect.fragment or redirect.username or redirect.password):
            raise ValueError("Native callback must be an explicit 127.0.0.1 loopback port and /callback")
        if not self.native_client_id or len(self.native_client_id) > 128:
            raise ValueError("Invalid native client ID")
        configured = (self.oidc_issuer, self.oidc_audience, self.oidc_jwks_url)
        if any(configured) and not all(configured):
            raise ValueError("Configure issuer, audience and JWKS URL together")
        if self.environment == "production" and not all(configured):
            raise ValueError("Production requires OIDC configuration")
        for url in (self.oidc_issuer, self.oidc_jwks_url, self.oidc_internal_url):
            if url:
                parsed = urlsplit(url)
                if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
                    raise ValueError("Invalid OIDC URL")
                if self.environment == "production" and parsed.scheme != "https":
                    raise ValueError("Production OIDC URLs must use HTTPS")
        for url in (self.public_url, self.relay_admin_url):
            if url and (urlsplit(url).scheme not in ("http", "https") or not urlsplit(url).hostname):
                raise ValueError("Invalid public or relay URL")
        if not self.admin_client_id or len(self.admin_client_id) > 128 or self.admin_client_id == self.native_client_id:
            raise ValueError("The dashboard needs its own Keycloak client")
        if self.relay_admin_url and len(self.relay_admin_token) < 32:
            raise ValueError("ACCOUNT_RELAY_ADMIN_TOKEN needs at least 32 characters")
        if self.oidc_internal_url and urlsplit(self.oidc_internal_url).path.rstrip('/') != urlsplit(self.oidc_issuer).path.rstrip('/'):
            raise ValueError("Internal OIDC route must retain the issuer realm path")
        if not 60 <= self.access_seconds <= 3600 or not 3600 <= self.refresh_seconds <= 7776000:
            raise ValueError("Invalid session lifetime")
        if not 1 <= self.rate_limit <= 10000 or not 60 <= self.admin_mfa_seconds <= 3600:
            raise ValueError("Invalid security limits")

    @property
    def admin_redirect_uri(self):
        return (self.public_url or "http://127.0.0.1:7385").rstrip("/") + "/admin/"

    @classmethod
    def from_env(cls):
        return cls(
            database_url=os.environ["ACCOUNT_DATABASE_URL"],
            digest_secret=os.environ["ACCOUNT_DIGEST_SECRET"],
            environment=os.getenv("ACCOUNT_ENV", "production"),
            oidc_issuer=os.getenv("ACCOUNT_OIDC_ISSUER", ""),
            oidc_audience=os.getenv("ACCOUNT_OIDC_AUDIENCE", ""),
            oidc_jwks_url=os.getenv("ACCOUNT_OIDC_JWKS_URL", ""),
            oidc_algorithm=os.getenv("ACCOUNT_OIDC_ALGORITHM", "RS256"),
            native_client_id=os.getenv("ACCOUNT_NATIVE_CLIENT_ID", "letsgo-native"),
            native_redirect_uri=os.getenv("ACCOUNT_NATIVE_REDIRECT_URI", "http://127.0.0.1:7386/callback"),
            oidc_internal_url=os.getenv("ACCOUNT_OIDC_INTERNAL_URL", ""),
            public_url=os.getenv("ACCOUNT_PUBLIC_URL", ""),
            admin_client_id=os.getenv("ACCOUNT_ADMIN_CLIENT_ID", "letsgo-admin"),
            admin_require_mfa=os.getenv("ACCOUNT_ADMIN_REQUIRE_MFA", "1") not in ("0", "false", "no"),
            dashboard_assets=os.getenv("ACCOUNT_DASHBOARD_ASSETS", ""),
            relay_admin_url=os.getenv("ACCOUNT_RELAY_ADMIN_URL", ""),
            relay_admin_token=os.getenv("ACCOUNT_RELAY_ADMIN_TOKEN", ""),
        )
