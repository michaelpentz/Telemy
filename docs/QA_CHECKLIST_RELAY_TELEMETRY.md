# QA Checklist: Per-Link Relay Telemetry

This checklist verifies the flow of per-link SRT stats from the Aegis Relay to the OBS Dock.

## 1. Prerequisites
- [ ] Aegis Relay (AWS) instance active.
- [ ] Bonding-capable source (e.g., Larix Broadcaster on a phone with 5G + WiFi) connected to the relay.
- [ ] Control Plane (Go) running and receiving health reports from the relay.
- [ ] Rust Core (`obs-telemetry-bridge`) polling the control plane.

## 2. Relay -> Control Plane Integration
- [ ] Verify `POST /api/v1/relay/health` from the relay contains the `links[]` array.
- [ ] Verify `links[]` correctly identifies active connections (e.g., T-Mobile, WiFi).
- [ ] Verify `bitrate_kbps`, `rtt_ms`, `packet_loss_pct`, and `jitter_ms` are present for each link.

## 3. Control Plane -> Rust Core Integration
- [ ] Verify `GET /api/v1/relay/active` response includes the `links[]` array.
- [ ] Verify the control plane correctly aggregates stats into the `bonded` object.

## 4. Rust Core -> IPC Bridge Integration
- [ ] Inspect the `status_snapshot` IPC payload.
- [ ] Verify `relay.links[]` matches the data from the control plane.
- [ ] Verify `connections.items[]` (in the bridge projection) is populated using link data.
    - [ ] `name` maps to `link.label`.
    - [ ] `bitrate` maps to `link.bitrate_kbps`.
    - [ ] `status` maps to `link.status`.

## 5. UI Rendering (Aegis Dock - Connections Section)
- [ ] **Link List:** Verify individual links appear as separate rows (e.g., T-Mobile, Verizon, WiFi).
- [ ] **Metrics:** Verify instantaneous bitrate is shown per link.
- [ ] **Signal Bars:** Verify signal bar counts reflect link quality (RTT/loss).
- [ ] **Dynamic Updates:**
    - [ ] Disconnect one link (e.g., turn off WiFi); verify its row shows `disconnected` or disappears.
    - [ ] Reconnect the link; verify it reappears in the list.
- [ ] **Bonded Aggregate:**
    - [ ] Verify the bonded bitrate matches the sum of all link bitrates.
    - [ ] Verify the "Bonded" status matches the relay's reported `bonded.health`.

## 7. Validation Status (v0.0.4)
- [x] **Relay E2E Telemetry:** PASSED (2026-03-05). Validated via IRL Pro bonded stream to AWS relay.
- [x] **Relay IPC Round-Trip:** Confirmed (Start → Provisioning → Active → Stop → Stopped).
- [x] **API Connectivity:** Confirmed (C++ RelayClient → Go Control Plane via HTTPS).
- [x] **Dock Telemetry Path:** Confirmed (SLS stats API → C++ → JSON Snapshot → CEF Injection → React UI).
- [ ] **Per-link Relay Telemetry:** PENDING (requires `srtla_rec` fork to expose per-link metadata).

## 8. Test Cases (v0.0.4)

### TC-RELAY-001: IRL Pro Bonded Stream
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
- **Notes**: Per-link stats (WiFi vs Cellular) are currently unavailable due to `srtla_rec` limitations.
