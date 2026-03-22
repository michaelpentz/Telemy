# QA Checklist: Relay Telemetry (v0.0.5 Native)

This checklist verifies the flow of relay stats from the managed relay to the OBS Dock in the v0.0.5 native architecture.

## 1. Prerequisites
- [ ] Aegis Relay (Advin VPS kc1) instance active.
- [ ] Bonding-capable source (e.g., Larix Broadcaster on a phone with 5G + WiFi) connected to the relay.
- [ ] OBS Studio running with `aegis-obs-plugin.dll` loaded.
- [ ] Aegis Dock (Telemy v0.0.5) visible in OBS.

## 2. Relay -> Plugin Integration (Aggregate)
- [ ] Verify `PollRelayStats()` in `relay_client.cpp` polls the relay's port 8090 every 2s.
- [ ] Verify `relay_ingest_bitrate_kbps`, `relay_rtt_ms`, `relay_pkt_loss`, and `relay_latency_ms` are injected into the top-level telemetry snapshot.

## 3. Relay -> Plugin Integration (Per-Link)
- [ ] Verify `PollPerLinkStats()` in `relay_client.cpp` polls the relay's port 5080 every 2s.
- [ ] Verify `relay_links[]` array is injected into the snapshot, including `addr`, `bytes`, `share_pct`, `last_ms_ago`, and `asn_org`.

## 4. Plugin -> Dock UI Integration
- [ ] Inspect the JSON snapshot pushed to the dock via CEF.
- [ ] Verify `relay.status` and `relay.region` match the active session.
- [ ] Verify `relay.links[]` matches the data from the relay's per-link stats API.
- [ ] **Security Check**: Verify `pair_token` and `relay_ws_token` are EXCLUDED from the snapshot sent to the dock.

## 5. UI Rendering (Aegis Dock - Relay Section)
- [ ] **Aggregate Card:** Verify bitrate bar shows real-time ingest throughput (e.g., 4.6 Mbps).
- [ ] **Per-Link BitrateBars:** Verify individual links appear as separate bars (e.g., T-Mobile, WiFi).
- [ ] **Stale Detection:** Disconnect a link; verify its bar fades (`opacity` drop) after 3s (`last_ms_ago > 3000`).
- [ ] **Carrier Labels:** Verify ASN-based labels (T-Mobile, AT&T, etc.) appear correctly.

## 7. Validation Status (v0.0.5)
- [x] **Relay E2E Telemetry:** PASSED (2026-03-05). Validated via IRL Pro bonded stream to AWS relay.
- [x] **Relay IPC Round-Trip:** Confirmed (Start → Provisioning → Active → Stop → Stopped).
- [x] **API Connectivity:** Confirmed (C++ RelayClient → Go Control Plane via HTTPS).
- [x] **Dock Telemetry Path:** Confirmed (SLS stats API → C++ → JSON Snapshot → CEF Injection → React UI).
- [x] **E2E Gap Fixes:** DONE (2026-03-20). Snapshot push on connect/disconnect, relay_host_masked, not_found for unknown IDs.
- [x] **MetricsCollector Background Thread:** DONE (2026-03-20). Polling moved off OBS render thread; Start()/Stop()/PollLoop() verified.
- [ ] **Per-link Relay Telemetry:** PENDING (requires `srtla_rec` fork to expose per-link metadata).

## 8. Test Cases (v0.0.5)

### TC-RELAY-001: IRL Pro Bonded Stream (Aggregate)
- **Description**: Verify aggregate stats for a bonded multi-link stream.
- **Setup**: IRL Pro connected via `srtla://` using both WiFi and Cellular links.
- **Steps**:
    1. Start relay via Dock UI.
    2. Start IRL Pro stream.
    3. Monitor "Relay Ingest" card in OBS Dock.
- **Expected Results**:
    - [x] Bitrate bar shows aggregate throughput (e.g., 4.6 Mbps).
    - [x] RTT, Latency, and Loss pills show real-time values from the relay.
    - [x] Stats update every 2 seconds.
- **Status**: PASSED (2026-03-05)

### TC-CONN-001: Multiple Simultaneous BYOR Connections
- **Description**: Verify two BYOR connections can be active simultaneously with independent telemetry.
- **Setup**: Two separate relay endpoints configured as BYOR connections.
- **Steps**:
    1. Add two BYOR connections via dock settings panel.
    2. Connect both via `relay_connect_direct`.
    3. Monitor dock Relay section.
- **Expected Results**:
    - [ ] Both connections show as active in dock.
    - [ ] Per-link bitrate bars appear under each connection independently.
    - [ ] Stats polling runs for each connection without interference.
    - [ ] Disconnecting one connection does not affect the other.

### TC-CONN-002: Connection Persistence Across OBS Restarts
- **Description**: Verify BYOR connections and their config survive OBS restarts.
- **Setup**: At least one BYOR connection configured with host/port/stream key.
- **Steps**:
    1. Configure a BYOR connection and connect.
    2. Restart OBS.
    3. Observe dock state on reload.
- **Expected Results**:
    - [ ] BYOR connection config is present after restart (loaded from `config.json`).
    - [ ] Sensitive fields (stream key) are restored from DPAPI vault.
    - [ ] Connection status is `idle` (not auto-connected) on restart.
    - [ ] Reconnecting via dock works without re-entering credentials.

### TC-CONN-003: Managed Connect/Disconnect Flow
- **Description**: Verify managed relay start/stop flow via ConnectionManager.
- **Setup**: Authenticated user with managed-tier entitlement.
- **Steps**:
    1. Click Start Relay in dock.
    2. Observe 6-step provision progress UI.
    3. Verify active relay display after `ready` step.
    4. Click Stop Relay.
- **Expected Results**:
    - [ ] Each provision step label shown in sequence (`launching_instance` -> `ready`).
    - [ ] Progress bar advances (N/6 counter).
    - [ ] Active relay stats appear after `ready`.
    - [ ] Stop transitions cleanly back to idle state.
    - [ ] No OBS freeze during start/stop (non-blocking worker threads).

### TC-CONN-004: Per-Link Telemetry With Multiple Carriers
- **Description**: Verify per-link stats show distinct carrier labels for bonded stream.
- **Setup**: IRL Pro connected via WiFi + cellular on managed relay.
- **Steps**:
    1. Start relay and connect IRL Pro with two active links.
    2. Monitor Relay section in dock.
- **Expected Results**:
    - [ ] Two `BitrateBar` elements appear, one per link.
    - [ ] Carrier labels show (e.g., "T-Mobile", "Cox") — not generic "Link 1/2".
    - [ ] `share_pct` for all links totals ~100%.
    - [ ] Stale link fades after 3s without data.
    - [ ] Link bars are nested under the correct relay connection.

### TC-RELAY-002: Per-Link Telemetry (Bonded)
- **Description**: Verify individual link contributions and health indicators.
- **Setup**: IRL Pro connected via `srtla://` using at least two active links (e.g., WiFi + 5G).
- **Steps**:
    1. Start relay via Dock UI.
    2. Start IRL Pro stream with two links active.
    3. Monitor "Relay Ingest" card in OBS Dock.
    4. Disconnect one link (e.g., disable WiFi on phone).
    5. Re-enable the link.
- **Expected Results**:
    - [ ] `/stats` (port 5080) returns valid JSON with `groups[]` and `connections[]`.
    - [ ] Individual `BitrateBar` elements appear for each active link in the Dock.
    - [ ] `share_pct` for all links adds to ~100%.
    - [ ] Link count badge (e.g., "2 LINKS") matches physical connections.
    - [ ] Disconnected link fades in UI (`opacity` drop) after 3 seconds (`last_ms_ago > 3000`).
    - [ ] Link disappears from UI if it remains stale for extended duration.
    - [ ] Re-enabled link reappears with correct stats.

