# Per-Link Relay Telemetry Design

**Date**: 2026-03-06
**Status**: Approved
**Scope**: Fork srtla_rec to expose per-link stats, consume in C++ plugin, display in dock UI

## Context

The aggregate relay telemetry (SLS stats) is validated and live. Streamers bonding WiFi + cellular via IRL Pro can see total ingest bitrate, RTT, and latency in the dock. But they can't see per-link breakdown — which connection is carrying load, which is underperforming, or when a link drops.

IRL Pro controls traffic distribution via user-configured weight sliders and automatic congestion windowing. The SRTLA protocol provides no receiver-to-sender feedback for weight adjustment. Our role is observe-and-report: show the actual per-link throughput so streamers can verify their IRL Pro weight settings are producing the expected result.

## Architecture

```
srtla_rec (forked)                    C++ Plugin                  Dock UI
 ├── UDP :5000 (ingest, unchanged)
 ├── per-link counters (hot path)
 └── HTTP :5080 /stats ──GET 2s──→ PollPerLinkStats() ──→ dock state JSON
                                      (WinHTTP, plain HTTP)    ──→ CEF injection
                                                                 ──→ per-link bars
```

No changes to the SRT forwarding path. The stats HTTP server runs on a separate thread, reads counters written by the single-threaded event loop. No locks on the hot path — worst case the HTTP handler reads a slightly stale counter value.

## srtla_rec Fork: Stats Module

### Data Model

Add to `conn_t`:
```c
uint64_t bytes_recv;      // total bytes received from this link
uint64_t pkts_recv;       // total packets received from this link
struct timespec first_seen; // connection registration time
struct timespec last_recv_ts; // high-resolution last packet time (for staleness)
```

Add to `conn_group_t`:
```c
uint64_t total_bytes_recv; // sum across all links (for share % calculation)
```

### Instrumentation Point

In `handle_srtla_data()`, after the existing `recvfrom()` and connection lookup, before `sendto()` to SLS:

```c
c->bytes_recv += n;
c->pkts_recv++;
clock_gettime(CLOCK_MONOTONIC, &c->last_recv_ts);
g->total_bytes_recv += n;
```

Four instructions added to the hot path. No branches, no allocations, no locks.

### HTTP Stats Endpoint

New file: `stats_server.c` (~200 lines)

- Listens on TCP port 5080 (configurable via `--stats_port` CLI arg)
- Runs in a pthread (created after event loop setup, before `epoll_wait` loop)
- Accepts one connection at a time (blocking accept, no epoll needed for stats)
- On `GET /stats`: iterates groups/connections, serializes JSON, writes response, closes socket
- On any other request: 404

Response format:
```json
{
  "ts": 1741276800,
  "groups": [
    {
      "id": "a1b2c3d4",
      "conn_count": 2,
      "total_bytes": 155339392,
      "connections": [
        {
          "addr": "192.168.1.5:45032",
          "bytes": 84291072,
          "pkts": 62438,
          "share_pct": 54.3,
          "last_ms_ago": 12,
          "uptime_s": 847
        },
        {
          "addr": "172.58.12.99:38201",
          "bytes": 71048320,
          "pkts": 52640,
          "share_pct": 45.7,
          "last_ms_ago": 8,
          "uptime_s": 845
        }
      ]
    }
  ]
}
```

Fields:
- `addr`: remote IP:port (identifies the link — WiFi vs cellular vs USB ethernet)
- `bytes`: total bytes received from this link since connection
- `pkts`: total packets received
- `share_pct`: this link's share of total group bytes (percentage)
- `last_ms_ago`: milliseconds since last packet (staleness indicator)
- `uptime_s`: seconds since this connection registered

### Thread Safety

srtla_rec is single-threaded (epoll event loop). The stats HTTP thread only reads counter values. On x86_64 and aarch64, aligned 64-bit reads/writes are atomic. No mutex needed. The HTTP thread may read a value mid-update at worst — producing a count off by one packet, which is acceptable for display purposes.

### CLI Arguments

Add `--stats_port <port>` (default: 5080). If set to 0, stats server is disabled.

### Build Changes

- New source file: `stats_server.c`, `stats_server.h`
- Link against `-lpthread` (already implicit on Linux)
- No new external dependencies

## Docker Image

### Custom Image: `ghcr.io/telemyapp/srtla-receiver`

Fork `OpenIRL/srtla-receiver` Dockerfile. Only change: point srtla git clone at our fork instead of `OpenIRL/srtla`. Everything else (SLS, supervisord, Alpine base) stays identical.

Update `conf/supervisord.conf` srtla command:
```
/usr/local/bin/srtla_rec --srtla_port=5000 --srt_hostname=localhost --srt_port=4001 --stats_port=5080
```

Expose port 5080/tcp in Dockerfile.

### CI

GitHub Actions workflow on the fork repo:
- On push to main: build multi-arch image (amd64 + arm64), push to ghcr.io
- Tag with commit SHA + `latest`

## Relay Provisioning Changes

### relay-user-data.sh

Update docker-compose fetch to use our custom image or override the receiver image:

```yaml
# In .env or docker-compose override
RECEIVER_IMAGE=ghcr.io/telemyapp/srtla-receiver:latest
```

Or simpler: replace the `docker-compose.yml` fetch with our own compose file that references our image.

### docker-compose.yml (new, bundled in user-data)

```yaml
services:
  sls-management-ui:
    image: ghcr.io/openirl/sls-management-ui:latest
    # ... (unchanged from OpenIRL)

  receiver:
    image: ghcr.io/telemyapp/srtla-receiver:latest
    ports:
      - "${SLS_STATS_PORT}:8080/tcp"
      - "${SRTLA_PORT}:5000/udp"
      - "${SRT_SENDER_PORT}:4001/udp"
      - "${SRT_PLAYER_PORT}:4000/udp"
      - "5080:5080/tcp"              # NEW: per-link stats
    volumes:
      - ./data:/var/lib/sls
```

### aws.go — Security Group

Add TCP port 5080 to relay SG ingress rules (same scope as 8090 — open to all, read-only, no auth).

## C++ Plugin Changes

### relay_client.h — New struct

```cpp
struct PerLinkStats {
    std::string addr;          // "192.168.1.5:45032"
    uint64_t bytes = 0;
    uint64_t pkts = 0;
    double share_pct = 0.0;
    uint32_t last_ms_ago = 0;
    uint32_t uptime_s = 0;
};

struct PerLinkSnapshot {
    bool available = false;
    int conn_count = 0;
    std::vector<PerLinkStats> links;
};
```

### relay_client.cpp — PollPerLinkStats()

New method alongside existing `PollRelayStats()`:
- `GET http://<relay_ip>:5080/stats`
- Parse JSON, populate `PerLinkSnapshot`
- Poll every 2 seconds (same cadence as SLS stats, can share the counter)

### metrics_collector.cpp — JSON injection

Add per-link array to dock state snapshot:
```json
{
  "relay_per_link_available": true,
  "relay_conn_count": 2,
  "relay_links": [
    {"addr": "192.168.1.5:45032", "bytes": 84291072, "pkts": 62438, "share_pct": 54.3, "last_ms_ago": 12, "uptime_s": 847},
    {"addr": "172.58.12.99:38201", "bytes": 71048320, "pkts": 52640, "share_pct": 45.7, "last_ms_ago": 8, "uptime_s": 845}
  ]
}
```

## Bridge JS Changes

### aegis-dock-bridge.js

Add to `getState().relay`:
```javascript
perLinkAvailable: !!snap.relay_per_link_available,
connCount: snap.relay_conn_count || 0,
links: (snap.relay_links || []).map(l => ({
  addr: l.addr,
  bytes: l.bytes,
  pkts: l.pkts,
  sharePct: l.share_pct,
  lastMsAgo: l.last_ms_ago,
  uptimeS: l.uptime_s,
})),
```

## Dock UI Changes

### Per-Link Throughput Bars

When `relay.perLinkAvailable && relay.links.length > 0`, render per-link bars in the Connections/Bitrate section:

```
WiFi  192.168.1.5     54%  3.2 Mbps  ██████████░░░
Cell  172.58.12.99    46%  1.4 Mbps  ██████░░░░░░░
```

Each bar shows:
- Link label (derived from IP range heuristic: private IP = WiFi/LAN, carrier IP = cellular)
- Traffic share percentage
- Current throughput (derived from bytes delta over poll interval)
- Color-coded bar (proportional to share)
- Staleness indicator: if `last_ms_ago > 3000`, dim the bar and show warning icon

### Link Drop Alert

When `relay.connCount` decreases between polls, show a brief toast/flash:
"Link disconnected — 1 connection remaining"

### Connection Count Badge

Show "2 links" or "1 link" badge next to the relay status indicator.

### Throughput Calculation

The stats endpoint returns cumulative `bytes`. The dock UI computes per-second throughput:
```javascript
const throughputKbps = ((link.bytes - prevLink.bytes) / pollIntervalSec) * 8 / 1000;
```

Previous values stored in a React ref, keyed by `link.addr`.

### IP Label Heuristic

Simple classification for display labels:
- `10.*`, `192.168.*`, `172.16-31.*` → "WiFi" or "LAN"
- Everything else → "Cell" or last two octets as fallback
- Not perfect, but useful for most IRL setups where WiFi = private IP, cellular = carrier IP

## Execution Lanes

### Lane 1: srtla_rec fork (C, new repo)
1. Fork OpenIRL/srtla → telemyapp/srtla
2. Add counter fields to conn_t/conn_group_t
3. Instrument handle_srtla_data()
4. Implement stats_server.c (HTTP /stats endpoint)
5. Add --stats_port CLI arg
6. Test locally (compile, run, curl /stats)

### Lane 2: Docker image (Dockerfile, CI)
1. Fork OpenIRL/srtla-receiver → telemyapp/srtla-receiver
2. Update Dockerfile to clone our srtla fork
3. Update supervisord.conf with --stats_port=5080
4. Expose port 5080
5. GitHub Actions: build + push to ghcr.io
6. Test: docker run, curl :5080/stats

### Lane 3: Relay provisioning (Go + shell)
1. Update relay-user-data.sh with custom compose / image reference
2. Update aws.go to open port 5080 in SG
3. Deploy control plane binary
4. Test: provision relay, verify port 5080 reachable

### Lane 4: C++ plugin (C++)
1. Add PerLinkStats/PerLinkSnapshot structs
2. Implement PollPerLinkStats()
3. Wire into dock state JSON
4. Build DLL

### Lane 5: Dock UI (JS/JSX)
1. Bridge JS: pass-through per-link data
2. Dock UI: per-link throughput bars, connection count badge
3. Throughput delta calculation
4. IP label heuristic
5. Link drop alert
6. Build bundle, test in dock-preview

### Dependencies
- Lane 1 → Lane 2 (need fork before Docker image)
- Lane 2 → Lane 3 (need image before provisioning)
- Lane 4 depends on Lane 3 being deployed (needs live relay with stats port)
- Lane 5 can start in parallel with Lane 4 (simulated data)
- Lane 4 + Lane 5 merge for E2E test

## Deferred (v2)

- Per-link jitter (inter-packet arrival variance)
- Per-link health score (composite metric)
- GeoIP carrier name lookup
- Link quality trend (rolling 30s window)
- "Consider reducing weight" recommendation in dock UI
- Per-link packet loss estimation (sequence gap analysis)

## AGPL-3.0 Compliance

srtla_rec is AGPL-3.0. Our fork must:
- Preserve original copyright notices
- Publish fork source on GitHub (public repo)
- Include AGPL-3.0 license
- Note modifications in source headers

This is satisfied by pushing to `github.com/telemyapp/srtla` as a public repo.
