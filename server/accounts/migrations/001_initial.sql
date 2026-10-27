CREATE TABLE accounts (
    id UUID PRIMARY KEY,
    issuer TEXT NOT NULL,
    subject TEXT NOT NULL,
    display_name VARCHAR(64) NOT NULL,
    role TEXT NOT NULL DEFAULT 'player' CHECK (role IN ('player', 'admin')),
    status TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'disabled')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (issuer, subject)
);
CREATE TABLE sessions (
    id UUID PRIMARY KEY,
    account_id UUID NOT NULL REFERENCES accounts(id),
    revoked BOOLEAN NOT NULL DEFAULT false,
    mfa_at TIMESTAMPTZ,
    expires_at TIMESTAMPTZ NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX sessions_account ON sessions(account_id);
CREATE TABLE credentials (
    digest CHAR(64) PRIMARY KEY,
    session_id UUID NOT NULL REFERENCES sessions(id),
    kind TEXT NOT NULL CHECK (kind IN ('access', 'refresh')),
    expires_at TIMESTAMPTZ NOT NULL,
    used BOOLEAN NOT NULL DEFAULT false
);
CREATE INDEX credentials_session ON credentials(session_id);
CREATE TABLE profiles (
    id UUID PRIMARY KEY,
    account_id UUID NOT NULL REFERENCES accounts(id),
    edition TEXT NOT NULL CHECK (edition IN ('pikachu', 'eevee')),
    label VARCHAR(64) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (id, account_id)
);
CREATE INDEX profiles_account ON profiles(account_id);
CREATE TABLE campaigns (
    id UUID PRIMARY KEY,
    title VARCHAR(120) NOT NULL,
    state TEXT NOT NULL DEFAULT 'draft' CHECK (state IN ('draft', 'published', 'paused')),
    published_at TIMESTAMPTZ,
    starts_at TIMESTAMPTZ NOT NULL,
    ends_at TIMESTAMPTZ NOT NULL,
    requires_code BOOLEAN NOT NULL DEFAULT false,
    editions JSONB NOT NULL,
    adapters JSONB NOT NULL,
    reward JSONB NOT NULL,
    reward_digest CHAR(64) NOT NULL,
    stock INTEGER CHECK (stock IS NULL OR stock >= 0),
    allocated INTEGER NOT NULL DEFAULT 0 CHECK (allocated >= 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (ends_at > starts_at),
    CHECK (stock IS NULL OR allocated <= stock)
);
CREATE TABLE gift_codes (
    id UUID PRIMARY KEY,
    campaign_id UUID NOT NULL REFERENCES campaigns(id),
    digest CHAR(64) NOT NULL UNIQUE,
    max_uses INTEGER NOT NULL CHECK (max_uses > 0),
    uses INTEGER NOT NULL DEFAULT 0 CHECK (uses >= 0 AND uses <= max_uses)
);
CREATE INDEX codes_campaign ON gift_codes(campaign_id);
CREATE TABLE claims (
    id UUID PRIMARY KEY,
    account_id UUID NOT NULL REFERENCES accounts(id),
    campaign_id UUID NOT NULL REFERENCES campaigns(id),
    profile_id UUID NOT NULL,
    code_id UUID REFERENCES gift_codes(id),
    adapter VARCHAR(64) NOT NULL,
    reward JSONB NOT NULL,
    reward_digest CHAR(64) NOT NULL,
    state TEXT NOT NULL DEFAULT 'allocated' CHECK (state IN ('allocated', 'client_reported_delivered')),
    receipt_digest CHAR(64),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    acknowledged_at TIMESTAMPTZ,
    UNIQUE (account_id, campaign_id),
    FOREIGN KEY (profile_id, account_id) REFERENCES profiles(id, account_id)
);
CREATE TABLE request_keys (
    account_id UUID NOT NULL REFERENCES accounts(id),
    operation TEXT NOT NULL,
    key VARCHAR(128) NOT NULL,
    request_digest CHAR(64) NOT NULL,
    response JSONB NOT NULL,
    PRIMARY KEY (account_id, operation, key)
);
CREATE TABLE audit_events (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    actor_id UUID REFERENCES accounts(id),
    action VARCHAR(64) NOT NULL,
    target_id UUID,
    request_id VARCHAR(36) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE TABLE rate_buckets (
    digest CHAR(64) NOT NULL,
    minute BIGINT NOT NULL,
    count INTEGER NOT NULL,
    PRIMARY KEY (digest, minute)
);
