-- Stream D cleanup: drop single-user BYOR relay columns and restore plan_tier constraint.
-- Supersedes 0011_byor_relay_config.sql which added these columns and the 'free' tier.

ALTER TABLE users
DROP COLUMN IF EXISTS byor_relay_host,
DROP COLUMN IF EXISTS byor_relay_port,
DROP COLUMN IF EXISTS byor_stream_id;

ALTER TABLE users
DROP CONSTRAINT IF EXISTS users_plan_tier_check,
ADD CONSTRAINT users_plan_tier_check CHECK (plan_tier IN ('starter', 'standard', 'pro'));
