-- 0003_add_relay_slug.sql
-- Adds a permanent relay slug to each user for DNS subdomain routing.
-- Each user gets a 6-char alphanumeric slug used as {slug}.relay.telemyapp.com

ALTER TABLE users ADD COLUMN relay_slug VARCHAR(8);

-- Backfill existing users with random 6-char slugs
UPDATE users SET relay_slug = substr(md5(random()::text), 1, 6) WHERE relay_slug IS NULL;

-- Enforce constraints
ALTER TABLE users ALTER COLUMN relay_slug SET NOT NULL;
ALTER TABLE users ADD CONSTRAINT users_relay_slug_unique UNIQUE (relay_slug);
