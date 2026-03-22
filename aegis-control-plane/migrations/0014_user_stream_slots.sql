-- 0014_user_stream_slots.sql
-- Per-user stream slots for multi-connection managed relays.
-- Each slot gets its own stream token for independent SRT ingest.

CREATE TABLE IF NOT EXISTS user_stream_slots (
    user_id      TEXT NOT NULL,
    slot_number  INTEGER NOT NULL,
    label        TEXT,
    stream_token TEXT NOT NULL,
    PRIMARY KEY (user_id, slot_number),
    UNIQUE (stream_token)
);
