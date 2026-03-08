# QA Checklist: Relay Telemetry (v0.0.4 Native)

This checklist verifies the flow of relay stats from the AWS Relay to the OBS Dock in the v0.0.4 native architecture.

## 1. Prerequisites
- [ ] Aegis Relay (AWS) instance active.
- [ ] Bonding-capable source (e.g., Larix Broadcaster on a phone with 5G + WiFi) connected to the relay.
- [ ] OBS Studio running with `aegis-obs-plugin.dll` loaded.
- [ ] Aegis Dock (Telemy v0.0.4) visible in OBS.

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

## 7. Validation Status (v0.0.4)
- [x] **Relay E2E Telemetry:** PASSED (2026-03-05). Validated via IRL Pro bonded stream to AWS relay.
- [x] **Relay IPC Round-Trip:** Confirmed (Start → Provisioning → Active → Stop → Stopped).
- [x] **API Connectivity:** Confirmed (C++ RelayClient → Go Control Plane via HTTPS).
- [x] **Dock Telemetry Path:** Confirmed (SLS stats API → C++ → JSON Snapshot → CEF Injection → React UI).
- [ ] **Per-link Relay Telemetry:** PENDING (requires `srtla_rec` fork to expose per-link metadata).

## 8. Test Cases (v0.0.4)

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

