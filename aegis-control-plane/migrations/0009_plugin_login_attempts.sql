create table if not exists plugin_login_attempts (
  id text primary key,
  poll_token_hash text not null,
  status text not null,
  user_id text references users(id) on delete set null,
  completed_session_id text references auth_sessions(id) on delete set null,
  client_platform text not null,
  client_version text,
  device_name text,
  deny_reason_code text,
  expires_at timestamptz not null,
  completed_at timestamptz,
  created_at timestamptz not null default now(),
  check (status in ('pending', 'completed', 'denied', 'expired'))
);

create unique index if not exists idx_plugin_login_attempts_poll_token_hash on plugin_login_attempts(poll_token_hash);
create index if not exists idx_plugin_login_attempts_expires on plugin_login_attempts(expires_at);
