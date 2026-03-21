-- Phase 3: shared relay pool and per-session assignments.
-- relay_pool: registered always-on relay servers.
-- relay_assignments: per-session allocations; released_at NULL = active.

CREATE TABLE relay_pool (
    server_id         TEXT PRIMARY KEY,
    provider          TEXT NOT NULL,
    host              TEXT NOT NULL,
    ip                INET NOT NULL,
    region            TEXT NOT NULL,
    status            TEXT NOT NULL DEFAULT 'active',
    current_sessions  INTEGER NOT NULL DEFAULT 0,
    max_sessions      INTEGER NOT NULL DEFAULT 10,
    created_at        TIMESTAMPTZ DEFAULT NOW(),
    last_health_check TIMESTAMPTZ,
    health_status     TEXT DEFAULT 'unknown'
);

CREATE TABLE relay_assignments (
    id            SERIAL PRIMARY KEY,
    user_id       TEXT NOT NULL,
    session_id    TEXT NOT NULL,
    connection_id TEXT,
    server_id     TEXT NOT NULL REFERENCES relay_pool(server_id),
    stream_token  TEXT NOT NULL,
    assigned_at   TIMESTAMPTZ DEFAULT NOW(),
    released_at   TIMESTAMPTZ,
    UNIQUE(session_id)
);

CREATE INDEX relay_assignments_session_idx ON relay_assignments(session_id);
CREATE INDEX relay_pool_region_status_idx ON relay_pool(region, status) WHERE status = 'active';
