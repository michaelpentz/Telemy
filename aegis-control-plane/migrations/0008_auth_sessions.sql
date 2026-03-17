create table if not exists auth_sessions (
  id text primary key,
  user_id text not null references users(id) on delete cascade,
  refresh_token_hash text not null unique,
  client_platform text not null,
  client_version text,
  device_name text,
  created_at timestamptz not null default now(),
  last_seen_at timestamptz not null default now(),
  expires_at timestamptz not null,
  revoked_at timestamptz
);

create index if not exists idx_auth_sessions_user on auth_sessions(user_id, created_at desc);
create index if not exists idx_auth_sessions_expires on auth_sessions(expires_at);
