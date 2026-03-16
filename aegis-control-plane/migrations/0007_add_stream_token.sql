ALTER TABLE users ADD COLUMN stream_token VARCHAR(12) UNIQUE;
UPDATE users SET stream_token = substr(md5(random()::text), 1, 8) WHERE stream_token IS NULL;
ALTER TABLE users ALTER COLUMN stream_token SET NOT NULL;
