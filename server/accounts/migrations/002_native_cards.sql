-- Native Mystery Gift cards (adapter lgpe-wb7-v1): each claim is served to the game as a one-time WB7 card whose id
-- the game records in the save's received flags (0..2047, never reused in one save). A profile is one save, so the ids
-- come from a per-profile counter in 1024..1999, clear of the official distributions PKHeX knows (1-4, 501-503,
-- 1001-1006, 2001: `tests/tools/gift-items events`).
ALTER TABLE profiles ADD COLUMN next_card_id INTEGER NOT NULL DEFAULT 1024 CHECK (next_card_id BETWEEN 1024 AND 2000);
ALTER TABLE claims ADD COLUMN card_id INTEGER CHECK (card_id BETWEEN 1024 AND 1999);
CREATE UNIQUE INDEX claims_profile_card ON claims(profile_id, card_id) WHERE card_id IS NOT NULL;
