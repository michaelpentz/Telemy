# Per-User Relay DNS Design

Date: 2026-03-05

## Overview

Add persistent per-user relay subdomains so IRL Pro users can configure their SRTLA ingest URL once and never change it. Each user gets a 6-char alphanumeric slug assigned at registration. On relay activation, the control plane creates a Cloudflare A record; on deactivation, it deletes it.

## User Experience

1. User signs up → gets slug `k7mx2p` → sees `k7mx2p.relay.telemyapp.com` in OBS dock
2. User enters `srtla://k7mx2p.relay.telemyapp.com:5000` in IRL Pro once
3. Every relay activation updates the DNS record to the new EC2 IP
4. IRL Pro connects without config change

## Data Flow

```
User registration:
  → Generate 6-char alphanumeric slug
  → INSERT users.relay_slug = 'k7mx2p' (UNIQUE constraint)
  → Collision: regenerate and retry

Relay start (after EC2 provisioned):
  → ActivateProvisionedSession() runs (existing)
  → Call Cloudflare API: PUT A record k7mx2p.relay.telemyapp.com → relay IP
  → TTL: 60s, proxied: false (DNS-only, grey cloud — UDP can't be proxied)
  → Add relay_hostname to session response

Relay stop:
  → StopSession() runs (existing)
  → Call Cloudflare API: DELETE A record for k7mx2p.relay.telemyapp.com
  → Terminate EC2 (existing)
```

## Components

### 1. DB Migration

```sql
ALTER TABLE users ADD COLUMN relay_slug VARCHAR(8) UNIQUE;
-- Backfill existing users
UPDATE users SET relay_slug = substr(md5(random()::text), 1, 6) WHERE relay_slug IS NULL;
ALTER TABLE users ALTER COLUMN relay_slug SET NOT NULL;
```

### 2. Cloudflare DNS Client (`internal/dns/cloudflare.go`)

New package, ~80 lines:
- `type Client struct` with zone ID and API token from env
- `CreateOrUpdateRelayRecord(slug, ip string) error` — upserts A record via Cloudflare API v4
- `DeleteRelayRecord(slug string) error` — deletes A record
- Base domain configurable: `CLOUDFLARE_RELAY_DOMAIN` env var (default `relay.telemyapp.com`)

Cloudflare API endpoints:
- List records: `GET /zones/{zone_id}/dns_records?name={slug}.relay.telemyapp.com`
- Create: `POST /zones/{zone_id}/dns_records`
- Update: `PUT /zones/{zone_id}/dns_records/{record_id}`
- Delete: `DELETE /zones/{zone_id}/dns_records/{record_id}`

### 3. Relay Start/Stop Integration

In `api/relay_handler.go` (or wherever start/stop orchestration lives):
- After `ActivateProvisionedSession`: look up user's slug, call `dns.CreateOrUpdateRelayRecord(slug, publicIP)`
- After `StopSession` + deprovision: call `dns.DeleteRelayRecord(slug)`
- DNS failures are logged but non-fatal (relay still works via raw IP)

### 4. Model Changes

- `model.Session` gets `RelayHostname string`
- `store.go` queries join `users.relay_slug` when building session
- API JSON response includes `"relay_hostname": "k7mx2p.relay.telemyapp.com"`

### 5. C++ Plugin

- `RelaySession` struct: add `relay_hostname` field
- `ParseSessionResponse()`: read `relay_hostname` from JSON
- Pass to dock snapshot as `relay_hostname`

### 6. Dock UI

- Ingest URL: show `srtla://{relay_hostname}:5000` when hostname is available, fall back to raw IP
- Display hostname prominently (this is what users copy to IRL Pro)

## Configuration

Env vars on control plane (`/etc/aegis-control-plane.env`):
- `CLOUDFLARE_DNS_TOKEN` — API token (already stored)
- `CLOUDFLARE_ZONE_ID` — Zone ID (already stored)
- `CLOUDFLARE_RELAY_DOMAIN` — Base domain (default: `relay.telemyapp.com`)

## Slug Properties

- 6 characters, lowercase alphanumeric (a-z, 0-9)
- 36^6 = 2.1 billion possibilities
- Assigned once at user creation, permanent (survives subscription lapses)
- UNIQUE constraint on DB column
- Collision handling: regenerate on unique violation, retry up to 3 times

## Security

- Cloudflare token scoped to DNS edit on telemyapp.com only
- DNS records are DNS-only (no Cloudflare proxy on UDP)
- Slug is not secret — knowing it only reveals the relay IP when active
- Relay still requires SRT stream ID for actual streaming

## Not In Scope

- OAuth / user registration flow
- Web dashboard showing slug
- User-chosen custom slugs
- Wildcard DNS fallback page
