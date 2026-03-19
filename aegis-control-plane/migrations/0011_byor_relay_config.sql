ALTER TABLE users
ADD COLUMN IF NOT EXISTS byor_relay_host VARCHAR(255),
ADD COLUMN IF NOT EXISTS byor_relay_port INTEGER DEFAULT 5000,
ADD COLUMN IF NOT EXISTS byor_stream_id VARCHAR(64);

ALTER TABLE users
DROP CONSTRAINT IF EXISTS users_plan_tier_check,
ADD CONSTRAINT users_plan_tier_check CHECK (plan_tier IN ('free', 'starter', 'standard', 'pro'));
