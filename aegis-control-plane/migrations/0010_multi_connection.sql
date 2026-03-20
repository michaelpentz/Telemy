-- Add connection_id to sessions so concurrent managed relays are scoped per plugin connection.
ALTER TABLE sessions ADD COLUMN IF NOT EXISTS connection_id TEXT;

-- Replace the single-active-session-per-user constraint with per-connection uniqueness.
DROP INDEX IF EXISTS sessions_one_active_per_user;

CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_active_per_user_null_connection
  ON sessions(user_id)
  WHERE connection_id IS NULL AND status IN ('provisioning', 'active', 'grace');

CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_active_per_user_connection
  ON sessions(user_id, connection_id)
  WHERE connection_id IS NOT NULL AND status IN ('provisioning', 'active', 'grace');

CREATE INDEX IF NOT EXISTS sessions_user_connection_idx
  ON sessions(user_id, connection_id)
  WHERE connection_id IS NOT NULL AND status IN ('provisioning', 'active', 'grace');
