# Relay Telemetry Design — Aggregate Stream Stats from SLS

**Date**: 2026-03-03
**Status**: Approved
**Scope**: Aggregate bonded stream telemetry (not per-link)

## Context

When a streamer bonds multiple connections through the srtla relay, OBS currently has no visibility into relay stream quality. The SLS (SRT Live Server) on the relay exposes aggregate stream stats via `GET :8090/stats/{player_id}?legacy=1`. srtla_rec (the bonding proxy) has no stats API, so per-link breakdown is deferred.

## Data Flow

```
SLS (relay:8090) <-- poll every 2s -- C++ Plugin (WinHTTP)
                                        |
                                   dock state JSON
                                        |
                                   CEF injection
                                        |
                                   Dock UI (bitrate, RTT, loss, latency)
```

## SLS Stats Endpoint

```
GET http://<relay-ip>:8090/stats/play_aegis?legacy=1
```

Response:
```json
{
  "status": "ok",
  "publishers": {
    "publish/live/live_aegis": {
      "bitrate": 3990,
      "rtt": 50.535,
      "msRcvBuf": 1935,
      "pktRcvLoss": 78458,
      "pktRcvDrop": 11,
      "bytesRcvLoss": 106702880,
      "bytesRcvDrop": 14960,
      "mbpsRecvRate": 5.158,
      "mbpsBandwidth": 9.276,
      "uptime": 1036,
      "latency": 120
    }
  }
}
```

No auth required. Rate limit: 300 req/min per IP.

## Changes by Component

### 1. Security Group — open port 8090 (`aws.go`)

Add port 8090 (SLS stats) to the relay security group ingress rules during provisioning. Currently only SRT ports (4000, 4001, 5000), management UI (3000), and SSH (22) are opened.

**Lane**: Codex

### 2. C++ Relay Client — `PollRelayStats()` (`relay_client.cpp`)

Add method to poll `http://<relay_public_ip>:8090/stats/play_aegis?legacy=1` via WinHTTP.

- Poll every 2 seconds while relay status == "active"
- Parse JSON response, extract publisher stats from nested `publishers` object
- Store parsed stats in `RelaySession` or adjacent struct
- On HTTP failure: flag `stats_available = false`, preserve last known values
- Stop polling when relay is inactive/stopped

**Lane**: Codex

### 3. Dock State Injection (`obs_plugin_entry.cpp`)

Include relay stats in the dock state JSON snapshot pushed to CEF:

```json
{
  "relay_ingest_bitrate_kbps": 8200,
  "relay_rtt_ms": 42,
  "relay_pkt_loss": 78458,
  "relay_pkt_drop": 11,
  "relay_recv_rate_mbps": 5.158,
  "relay_bandwidth_mbps": 9.276,
  "relay_latency_ms": 120,
  "relay_uptime_seconds": 1036,
  "relay_stats_available": true
}
```

**Lane**: Codex

### 4. Bridge JS — pass-through (`aegis-dock-bridge.js`)

Map snapshot fields into `getState().relay`:

```js
relay: {
  // existing fields unchanged
  enabled, active, status, region, pairToken, wsUrl, licensed, lastError, lastErrorTs,

  // new from SLS stats
  ingestBitrateKbps: snap.relay_ingest_bitrate_kbps || 0,
  rttMs: snap.relay_rtt_ms || null,
  pktLoss: snap.relay_pkt_loss || 0,
  pktDrop: snap.relay_pkt_drop || 0,
  recvRateMbps: snap.relay_recv_rate_mbps || null,
  bandwidthMbps: snap.relay_bandwidth_mbps || null,
  latencyMs: snap.relay_latency_ms || null,
  uptimeSeconds: snap.relay_uptime_seconds || 0,
  statsAvailable: !!snap.relay_stats_available,
}
```

**Lane**: Claude Code

### 5. Dock UI (`aegis-dock.jsx`)

Replace placeholder per-link bars in Connections section with relay quality card:

- Bitrate gauge: current ingest kbps, health color based on ratio to estimated bandwidth
- RTT: round-trip time in ms
- Packet loss: drop count
- Latency: end-to-end delay in ms
- Uptime: session duration
- Hidden when `!statsAvailable`

Existing per-link bars (`conns[0]`, `conns[1]`) hidden — no per-link data available.

**Lane**: Claude Code

## Deferred

- Per-link telemetry: requires forking srtla_rec to add HTTP stats endpoint
- Per-link connection count: could parse srtla_rec stderr logs (fragile)
- Auto-scaling relay capacity based on link count
