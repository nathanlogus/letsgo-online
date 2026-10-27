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
    rate_limit: int = 120

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
        if self.oidc_internal_url and urlsplit(self.oidc_internal_url).path.rstrip('/') != urlsplit(self.oidc_issuer).path.rstrip('/'):
            raise ValueError("Internal OIDC route must retain the issuer realm path")
        if not 60 <= self.access_seconds <= 3600 or not 3600 <= self.refresh_seconds <= 7776000:
            raise ValueError("Invalid session lifetime")
        if not 1 <= self.rate_limit <= 10000 or not 60 <= self.admin_mfa_seconds <= 3600:
            raise ValueError("Invalid security limits")

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
        )
