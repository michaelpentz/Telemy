# Relay Telemetry Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Surface aggregate SLS stream stats (bitrate, RTT, packet loss, latency) from the relay into the OBS dock UI.

**Architecture:** C++ plugin polls SLS stats endpoint directly via WinHTTP when relay is active. Stats flow through the existing dock state JSON → bridge JS → React dock. No control plane changes needed.

**Tech Stack:** C++ (WinHTTP, QJsonDocument), JavaScript (bridge pass-through), React JSX (dock UI)

---

## Pre-requisite: Open port 8090 on relay security group

**Manual step** — add TCP port 8090 ingress rule to SG `sg-0da8cf50c2fd72518` from `0.0.0.0/0`.
This is the SLS stats API port. Run once via AWS Console or CLI:

```bash
aws ec2 authorize-security-group-ingress \
  --group-id sg-0da8cf50c2fd72518 \
  --protocol tcp --port 8090 --cidr 0.0.0.0/0 \
  --region us-west-2
```

SLS stats endpoint is read-only, no auth, rate-limited to 300 req/min. Low risk.

---

## Lane A: C++ Backend (Codex)

### Task 1: Add RelayStats struct to relay_client.h

**Files:**
- Modify: `obs-plugin/src/relay_client.h`

**Step 1: Add RelayStats struct after RelaySession (line 22)**

```cpp
struct RelayStats {
    bool     available = false;
    uint32_t bitrate_kbps = 0;        // SLS: bitrate
    double   rtt_ms = 0.0;            // SLS: rtt
    uint64_t pkt_loss = 0;            // SLS: pktRcvLoss
    uint64_t pkt_drop = 0;            // SLS: pktRcvDrop
    double   recv_rate_mbps = 0.0;    // SLS: mbpsRecvRate
    double   bandwidth_mbps = 0.0;    // SLS: mbpsBandwidth
    uint32_t latency_ms = 0;          // SLS: latency
    uint32_t uptime_seconds = 0;      // SLS: uptime
};
```

**Step 2: Add stats polling members to RelayClient class**

After existing public methods (around line 45), add:

```cpp
// SLS stats polling
void PollRelayStats(const std::string& relay_ip);
RelayStats CurrentStats() const;
```

After existing private members (around line 65), add:

```cpp
RelayStats         stats_;
mutable std::mutex stats_mutex_;
```

**Step 3: Build to verify compilation**

Run: `cmake --build build-obs-cef --config Release`
Expected: compiles (no new implementations yet, just declarations)

**Step 4: Commit**

```bash
git add obs-plugin/src/relay_client.h
git commit -m "feat: add RelayStats struct for SLS telemetry"
```

---

### Task 2: Implement PollRelayStats in relay_client.cpp

**Files:**
- Modify: `obs-plugin/src/relay_client.cpp`

**Step 1: Implement PollRelayStats()**

Add after `EmergencyStop()` (around line 313):

```cpp
void RelayClient::PollRelayStats(const std::string& relay_ip) {
    if (relay_ip.empty()) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    // GET http://<relay_ip>:8090/stats/play_aegis?legacy=1
    std::string host = relay_ip;
    int port = 8090;
    std::string path = "/stats/play_aegis?legacy=1";

    auto resp = client_.Get(host, port, path, "", false /*no TLS*/);
    if (resp.status_code != 200 || resp.body.empty()) {
        blog(LOG_DEBUG,
            "[aegis-relay] stats poll failed: host=%s status=%d",
            host.c_str(), resp.status_code);
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    // Parse legacy JSON: { "status":"ok", "publishers": { "<key>": { ... } } }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(resp.body.c_str(), (int)resp.body.size()), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject pubs = root.value("publishers").toObject();
    if (pubs.isEmpty()) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    // Take the first (and typically only) publisher
    QJsonObject pub = pubs.begin()->toObject();

    RelayStats s;
    s.available        = true;
    s.bitrate_kbps     = (uint32_t)pub.value("bitrate").toInt(0);
    s.rtt_ms           = pub.value("rtt").toDouble(0.0);
    s.pkt_loss         = (uint64_t)pub.value("pktRcvLoss").toDouble(0);
    s.pkt_drop         = (uint64_t)pub.value("pktRcvDrop").toDouble(0);
    s.recv_rate_mbps   = pub.value("mbpsRecvRate").toDouble(0.0);
    s.bandwidth_mbps   = pub.value("mbpsBandwidth").toDouble(0.0);
    s.latency_ms       = (uint32_t)pub.value("latency").toInt(0);
    s.uptime_seconds   = (uint32_t)pub.value("uptime").toInt(0);

    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_ = s;
    }

    blog(LOG_DEBUG,
        "[aegis-relay] stats poll ok: bitrate=%u rtt=%.1f loss=%llu drop=%llu latency=%u",
        s.bitrate_kbps, s.rtt_ms, s.pkt_loss, s.pkt_drop, s.latency_ms);
}

RelayStats RelayClient::CurrentStats() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    return stats_;
}
```

**Note for Codex:** The `client_` member is the existing `HttpsClient` instance. Check its `Get()` method signature — it may need a `Get(host, port, path, auth_header, use_tls)` overload or use `Request("GET", ...)`. Adapt the call to match the existing HttpsClient API. The key requirement is plain HTTP (not HTTPS) to port 8090.

**Step 2: Build to verify compilation**

Run: `cmake --build build-obs-cef --config Release`
Expected: compiles clean

**Step 3: Commit**

```bash
git add obs-plugin/src/relay_client.cpp
git commit -m "feat: implement PollRelayStats from SLS stats API"
```

---

### Task 3: Wire relay stats into dock state JSON

**Files:**
- Modify: `obs-plugin/src/metrics_collector.cpp` (BuildStatusSnapshotJson, around line 450)
- Modify: `obs-plugin/src/metrics_collector.h` (update method signature)
- Modify: `obs-plugin/src/obs_plugin_entry.cpp` (tick callback, around line 968)

**Step 1: Add RelayStats parameter to BuildStatusSnapshotJson**

In `metrics_collector.h`, update the method signature to accept `const RelayStats*`:

```cpp
std::string BuildStatusSnapshotJson(
    const std::string& mode,
    const std::string& health,
    const std::string& relay_status,
    const std::string& relay_region,
    const aegis::RelaySession* relay_session,
    const aegis::RelayStats* relay_stats = nullptr) const;
```

**Step 2: Add relay stats fields to JSON output**

In `metrics_collector.cpp`, after the existing relay session fields (around line 460, after the relay object closing `}`), add top-level relay stats fields:

```cpp
// ── Relay telemetry (from SLS stats) ─────────────────────────────────
if (relay_stats && relay_stats->available) {
    os << "\"relay_ingest_bitrate_kbps\":" << relay_stats->bitrate_kbps << ",";
    os << "\"relay_rtt_ms\":" << relay_stats->rtt_ms << ",";
    os << "\"relay_pkt_loss\":" << relay_stats->pkt_loss << ",";
    os << "\"relay_pkt_drop\":" << relay_stats->pkt_drop << ",";
    os << "\"relay_recv_rate_mbps\":" << relay_stats->recv_rate_mbps << ",";
    os << "\"relay_bandwidth_mbps\":" << relay_stats->bandwidth_mbps << ",";
    os << "\"relay_latency_ms\":" << relay_stats->latency_ms << ",";
    os << "\"relay_uptime_seconds\":" << relay_stats->uptime_seconds << ",";
    os << "\"relay_stats_available\":true,";
} else {
    os << "\"relay_stats_available\":false,";
}
```

**Step 3: Call PollRelayStats and pass stats in tick callback**

In `obs_plugin_entry.cpp`, in the tick callback (around line 968), after getting the relay session:

```cpp
// Poll SLS stats if relay is active
const aegis::RelayStats* relay_stats_ptr = nullptr;
aegis::RelayStats relay_stats;
if (g_relay && g_relay->HasActiveSession() && relay_session_ptr) {
    // Poll every ~2 seconds (4 ticks at 500ms)
    static int stats_poll_counter = 0;
    if (++stats_poll_counter >= 4) {
        stats_poll_counter = 0;
        g_relay->PollRelayStats(relay_session_ptr->public_ip);
    }
    relay_stats = g_relay->CurrentStats();
    relay_stats_ptr = &relay_stats;
}

std::string json =
    g_metrics.BuildStatusSnapshotJson(mode, health, relay_status, relay_region, relay_session_ptr, relay_stats_ptr);
```

**Step 4: Build to verify compilation**

Run: `cmake --build build-obs-cef --config Release`
Expected: compiles clean

**Step 5: Commit**

```bash
git add obs-plugin/src/metrics_collector.cpp obs-plugin/src/metrics_collector.h obs-plugin/src/obs_plugin_entry.cpp
git commit -m "feat: wire relay SLS stats into dock state JSON snapshot"
```

---

## Lane B: Dock UI (Claude Code)

### Task 4: Bridge JS — relay stats pass-through

**Files:**
- Modify: `E:/Code/telemyapp/aegis-dock-bridge.js` (getState relay section, lines 297-317)

**Step 1: Add relay stats fields to getState().relay**

In the relay object inside `buildState()`, after `lastErrorTs` (line 316), add:

```javascript
// SLS relay telemetry (aggregate bonded stream)
ingestBitrateKbps: snap.relay_ingest_bitrate_kbps || 0,
rttMs: snap.relay_rtt_ms != null ? snap.relay_rtt_ms : null,
pktLoss: snap.relay_pkt_loss || 0,
pktDrop: snap.relay_pkt_drop || 0,
recvRateMbps: snap.relay_recv_rate_mbps != null ? snap.relay_recv_rate_mbps : null,
bandwidthMbps: snap.relay_bandwidth_mbps != null ? snap.relay_bandwidth_mbps : null,
latencyMs: snap.relay_latency_ms != null ? snap.relay_latency_ms : null,
uptimeSeconds: snap.relay_uptime_seconds || 0,
statsAvailable: !!snap.relay_stats_available,
```

Also update the existing `latencyMs` line (currently `snap.rtt_ms`) to prefer relay RTT:

```javascript
latencyMs: snap.relay_rtt_ms != null ? snap.relay_rtt_ms : (snap.rtt_ms != null ? snap.rtt_ms : null),
```

**Step 2: Rebuild dock app bundle**

Run: `esbuild aegis-dock-entry.jsx --bundle --format=iife --jsx=automatic --outfile=aegis-dock-app.js --target=es2020 --minify`

**Step 3: Commit**

```bash
git add aegis-dock-bridge.js aegis-dock-app.js
git commit -m "feat: pass relay SLS stats through dock bridge"
```

---

### Task 5: Dock UI — relay quality metrics display

**Files:**
- Modify: `E:/Code/telemyapp/aegis-dock.jsx`

**Step 1: Add relay quality card to RELAY ACTIVE section**

In the RELAY ACTIVE section (around line 2850), after the existing Region/Latency/Uptime rows, add a relay quality sub-card that shows when `relay.statsAvailable`:

```jsx
{/* Relay Stream Quality */}
{relay.statsAvailable && (
  <div style={{
    background: "var(--theme-surface, #1e2024)",
    borderRadius: 6,
    padding: "8px 10px",
    marginTop: 6,
  }}>
    <div style={{ fontSize: 9, fontWeight: 700, textTransform: "uppercase",
      color: "var(--theme-text-muted, #8b8f98)", marginBottom: 6,
      letterSpacing: "0.5px" }}>
      Relay Ingest
    </div>
    {/* Bitrate bar */}
    <div style={{ marginBottom: 4 }}>
      <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 2 }}>
        <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)" }}>Bitrate</span>
        <span style={{ fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontWeight: 600 }}>
          {relay.ingestBitrateKbps >= 1000
            ? `${(relay.ingestBitrateKbps / 1000).toFixed(1)} Mbps`
            : `${relay.ingestBitrateKbps} kbps`}
        </span>
      </div>
      <div style={{ height: 4, borderRadius: 2,
        background: "var(--theme-border, #3a3d45)", overflow: "hidden" }}>
        <div style={{
          height: "100%", borderRadius: 2, transition: "width 0.6s ease",
          width: `${Math.min(100, relay.bandwidthMbps > 0
            ? (relay.recvRateMbps / relay.bandwidthMbps) * 100 : 50)}%`,
          background: relay.bandwidthMbps > 0 && relay.recvRateMbps / relay.bandwidthMbps > 0.9
            ? "#da3633" : relay.bandwidthMbps > 0 && relay.recvRateMbps / relay.bandwidthMbps > 0.7
            ? "#d29922" : "#2ea043",
        }} />
      </div>
    </div>
    {/* Stats row */}
    <div style={{ display: "flex", gap: 8, marginTop: 6 }}>
      <StatPill label="RTT" value={relay.rttMs != null ? `${Math.round(relay.rttMs)}ms` : "—"}
        color={relay.rttMs > 100 ? "#da3633" : relay.rttMs > 50 ? "#d29922" : "#2ea043"} />
      <StatPill label="Loss" value={relay.pktDrop > 0 ? String(relay.pktDrop) : "0"}
        color={relay.pktDrop > 100 ? "#da3633" : relay.pktDrop > 10 ? "#d29922" : "#2ea043"} />
      <StatPill label="Latency" value={relay.latencyMs != null ? `${relay.latencyMs}ms` : "—"}
        color={relay.latencyMs > 200 ? "#da3633" : relay.latencyMs > 100 ? "#d29922" : "#2ea043"} />
    </div>
  </div>
)}
```

**Step 2: Add StatPill helper component**

Add near other small components (around line 1400):

```jsx
function StatPill({ label, value, color }) {
  return (
    <div style={{ flex: 1, textAlign: "center" }}>
      <div style={{ fontSize: 8, color: "var(--theme-text-muted, #8b8f98)",
        textTransform: "uppercase", letterSpacing: "0.4px" }}>{label}</div>
      <div style={{ fontSize: 11, fontWeight: 600, color: color || "var(--theme-text, #e0e2e8)" }}>
        {value}
      </div>
    </div>
  );
}
```

**Step 3: Update existing Relay Latency row to use SLS data**

In the ACTIVE section, the existing "Relay Latency" row (around line 2865) already reads `relay.latencyMs`. This will now be populated by SLS RTT data via the bridge update in Task 4. No change needed.

**Step 4: Wire relayBondedKbps to SLS ingest bitrate**

In the bitrate section (around line 1894), update `relayBondedKbps` to use the new relay ingest bitrate:

```javascript
const relayBondedKbps = relay.statsAvailable ? relay.ingestBitrateKbps : (bitrate.relayBondedKbps || bondedKbps);
```

**Step 5: Hide per-link bars when no per-link data**

In the bitrate section (around line 2693), update the per-link condition to also require actual connection data:

```jsx
{relayActive && conns.length >= 2 ? (
```

No change needed — `conns` will remain empty (no per-link data), so the bars won't render. The bonded and relay ingest bars should still show. Update the fallback to show relay ingest bar even without per-link:

```jsx
{relayActive && relay.statsAvailable && (
  <BitrateBar value={useAnimatedValue(relay.ingestBitrateKbps, 600)}
    max={relay.bandwidthMbps ? relay.bandwidthMbps * 1000 : 15000}
    color="#5ba3f5" label="RELAY INGEST" />
)}
```

**Step 6: Rebuild dock app bundle**

Run: `esbuild aegis-dock-entry.jsx --bundle --format=iife --jsx=automatic --outfile=aegis-dock-app.js --target=es2020 --minify`

**Step 7: Test in dock-preview**

Run: `cd dock-preview && npx vite --port 5199`
Verify: relay quality card renders with simulated data, colors change with thresholds.

**Step 8: Commit**

```bash
git add aegis-dock.jsx aegis-dock-app.js
git commit -m "feat: relay quality metrics display in dock UI"
```

---

## Execution Order

Lanes A and B are independent and can run in parallel:

- **Lane A (Tasks 1-3)**: Codex — C++ relay stats polling + JSON injection
- **Lane B (Tasks 4-5)**: Claude Code — bridge pass-through + dock UI

Both lanes merge at the dock state JSON contract. Test E2E after both lanes complete by deploying DLL + dock assets to OBS and activating a relay.

## Verification

After both lanes complete:
1. Activate relay from dock UI
2. Start streaming from IRL Pro to relay
3. Check OBS logs for `[aegis-relay] stats poll ok: bitrate=... rtt=...`
4. Verify dock shows relay quality card with live updating stats
5. Stop stream — verify `statsAvailable` goes false, card hides
