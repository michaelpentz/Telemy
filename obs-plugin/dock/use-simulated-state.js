import { useState, useEffect, useCallback, useMemo } from "react";
import { SIM_SCENES, SIM_SETTING_DEFS, SIM_EVENTS, OBS_YAMI_GREY_DEFAULTS, ENGINE_STATES, DEFAULT_AUTO_SCENE_RULES } from "./constants.js";
import { genRequestId } from "./utils.js";

// ---------------------------------------------------------------------------
// Simulation layer — produces exact bridge getState() shape
// ---------------------------------------------------------------------------
export function useSimulatedState() {
  const [mode, setMode] = useState("studio");
  const [simRelayActive, setSimRelayActive] = useState(false);
  const [simRelayData, setSimRelayData] = useState({
    relayHostname: "byor.telemy.test",
    ingestUrl: "srtla://byor.telemy.test:5000",
    pairToken: "ABCD-1234-EFGH",
    region: "custom",
    byorEnabled: true,
    byorRelayHost: "byor.telemy.test",
    byorRelayPort: 5000,
    byorStreamId: "live/custom",
  });
  const [activeSceneId, setActiveSceneId] = useState("scene_1");
  const [pendingSceneId, setPendingSceneId] = useState(null);
  const [elapsed, setElapsed] = useState(3847);
  const [sim1, setSim1] = useState(4200);
  const [sim2, setSim2] = useState(2800);
  const [settingValues, setSettingValues] = useState({
    auto_scene_switch: true,
    low_quality_fallback: true,
    manual_override: false,
    chat_bot: null,
    alerts: true,
  });

  // Animate sim data
  useEffect(() => {
    const iv = setInterval(() => {
      setSim1(prev => Math.max(500, Math.min(6000, prev + (Math.random() - 0.48) * 800)));
      setSim2(prev => Math.max(200, Math.min(4000, prev + (Math.random() - 0.5) * 600)));
      setElapsed(prev => prev + 3);
    }, 3000);
    return () => clearInterval(iv);
  }, []);

  // Build state matching exact bridge shape
  const state = useMemo(() => ({
    header: {
      title: "AEGIS",
      subtitle: "OBS + Core IPC Dock",
      mode,
      modes: ["studio", "irl"],
      version: "v0.0.5",
    },
    live: {
      isLive: true,
      elapsedSec: elapsed,
    },
    scenes: {
      items: SIM_SCENES,
      activeSceneId,
      pendingSceneId,
      autoSwitchArmed: !settingValues.manual_override,
    },
    connections: {
      items: [
        { name: "SIM 1 \u2014 T-Mobile", type: "5G",      signal: 4, bitrate: sim1, status: "connected" },
        { name: "SIM 2 \u2014 Verizon",  type: "LTE",     signal: 3, bitrate: sim2, status: "connected" },
        { name: "WiFi",                   type: "802.11ac", signal: 0, bitrate: 0,    status: "disconnected" },
      ],
    },
    relay_connections: [
      {
        id: "sim-byor-1",
        name: "Main Cam \u2192 My VPS",
        type: "byor",
        status: "connected",
        relay_host_masked: "my-relay.e***.com",
        relay_host: "my-relay.example.com",
        relay_port: 5000,
        stream_id: "live/stream123",
        stats: { bitrate_kbps: Math.round(sim1 * 0.9), rtt_ms: 45, available: true },
        per_link: { available: false },
      },
      {
        id: "sim-managed-1",
        name: "Backpack \u2192 Telemy US-East",
        type: "managed",
        status: "connected",
        managed_region: "us-east",
        session_id: "ses_sim_abc123",
        relay_ip: "0.0.0.0",
        stats: { bitrate_kbps: Math.round(sim2 * 1.1), rtt_ms: 31, available: true },
        per_link: {
          available: true,
          links: [
            { carrier: "Cox",      bitrate_kbps: Math.round(sim2 * 0.58), rtt_ms: 28 },
            { carrier: "T-Mobile", bitrate_kbps: Math.round(sim2 * 0.42), rtt_ms: 38 },
          ],
        },
      },
      {
        id: "sim-byor-2",
        name: "Backup \u2192 BYOR EU",
        type: "byor",
        status: "idle",
        relay_host_masked: "eu-relay.e***.com",
        relay_host: "eu-relay.example.com",
        relay_port: 5000,
        stream_id: "",
        stats: { bitrate_kbps: 0, rtt_ms: 0, available: false },
        per_link: { available: false },
      },
    ],
    bitrate: {
      bondedKbps: sim1 + sim2,
      relayBondedKbps: sim1 + sim2,
      maxPerLinkKbps: 6000,
      maxBondedKbps: 12000,
      lowThresholdMbps: 1.5,
      brbThresholdMbps: 0.5,
      outputs: [
        { platform: "Twitch", kbps: Math.max(800, Math.floor((sim1 + sim2) * 0.55)), status: "active" },
        { platform: "YouTube", kbps: Math.max(600, Math.floor((sim1 + sim2) * 0.45)), status: "active" },
        { platform: "Kick", kbps: Math.max(300, Math.floor((sim1 + sim2) * 0.25)), status: simRelayActive ? "active" : "idle" },
      ],
    },
    outputs: { groups: [], hidden: [] },
    relay: {
      licensed: true,
      active: simRelayActive,
      enabled: simRelayActive, // backward compat
      status: simRelayActive ? "active" : "inactive",
      byorEnabled: simRelayData.byorEnabled === true,
      byorRelayHost: simRelayData.byorRelayHost || "",
      byorRelayPort: simRelayData.byorRelayPort || 5000,
      byorStreamId: simRelayData.byorStreamId || "",
      region: simRelayActive ? (simRelayData.region || "us-east-1") : "us-east-1",
      latencyMs: simRelayActive ? 42 : null,
      uptimeSec: simRelayActive ? elapsed : 0,
      graceRemainingSeconds: null,
      relayHostname: simRelayActive ? (simRelayData.relayHostname || null) : null,
      ingestUrl: simRelayActive ? (simRelayData.ingestUrl || null) : null,
      pairToken: simRelayActive ? (simRelayData.pairToken || null) : null,
      graceWindowSeconds: null,
      maxSessionSeconds: null,
      // SLS aggregate stats (simulated)
      statsAvailable: simRelayActive,
      ingestBitrateKbps: simRelayActive ? sim1 + sim2 : 0,
      rttMs: simRelayActive ? 42 : null,
      relayLatencyMs: simRelayActive ? 120 : null,
      pktLoss: simRelayActive ? 234 : 0,
      pktDrop: simRelayActive ? 3 : 0,
      lossRate: simRelayActive ? 2 : 0,
      recvRateMbps: simRelayActive ? (sim1 + sim2) / 1000 : null,
      bandwidthMbps: simRelayActive ? 12.0 : null,
      uptimeSeconds: simRelayActive ? elapsed : 0,
      // Per-link simulated data
      perLinkAvailable: simRelayActive,
      connCount: simRelayActive ? 2 : 0,
      links: simRelayActive ? [
        { addr: "192.168.1.105:45032", bytes: Math.floor(sim1 * elapsed * 0.125), pkts: Math.floor(sim1 * elapsed * 0.125 / 1350), sharePct: sim1 / (sim1 + sim2) * 100, lastMsAgo: 12, uptimeS: elapsed },
        { addr: "198.51.100.99:38201", bytes: Math.floor(sim2 * elapsed * 0.125), pkts: Math.floor(sim2 * elapsed * 0.125 / 1350), sharePct: sim2 / (sim1 + sim2) * 100, lastMsAgo: 8, uptimeS: elapsed },
      ] : [],
    },
    auth: {
      authenticated: true,
      hasTokens: true,
      user: {
        id: "sim-user",
        email: "preview@telemyapp.test",
        display_name: "Preview Operator",
      },
      entitlement: {
        relay_access_status: "enabled",
        reason_code: "",
        plan_tier: "starter",
        plan_status: "active",
        max_concurrent_conns: 1,
        active_managed_conns: 1,
      },
      usage: {
        included_seconds: 0,
        consumed_seconds: 0,
        remaining_seconds: 0,
        overage_seconds: 0,
      },
      activeRelay: null,
      login: { pending: false, poll_interval_seconds: 3 },
      lastErrorCode: null,
      lastErrorMessage: null,
    },
    failover: {
      health: "healthy",
      state: simRelayActive ? "IRL_ACTIVE" : "STUDIO",
      states: ENGINE_STATES.map(s => s.id),
      responseBudgetMs: 800,
      lastFailoverLabel: null,
      totalFailoversLabel: null,
    },
    settings: {
      items: SIM_SETTING_DEFS.map(d => ({
        ...d,
        value: settingValues[d.key] ?? null,
      })),
    },
    events: SIM_EVENTS,
    pipe: { status: "ok", label: "IPC: OK" },
    meta: {
      coreVersion: null,
      pluginVersion: null,
      lastPongUnixMs: null,
      lastHelloAckUnixMs: null,
    },
    theme: OBS_YAMI_GREY_DEFAULTS,
  }), [mode, elapsed, activeSceneId, pendingSceneId, sim1, sim2, settingValues, simRelayActive, simRelayData]);

  // Sim action handler — mirrors bridge sendDockAction behavior
  const sendAction = useCallback((action) => {
    switch (action.type) {
      case "switch_scene": {
        const targetId = action.sceneId || SIM_SCENES.find(s => s.name === action.sceneName)?.id;
        if (!targetId) return { ok: false, error: "scene_not_found" };
        setPendingSceneId(targetId);
        setTimeout(() => {
          setActiveSceneId(targetId);
          setPendingSceneId(null);
        }, 400);
        return { ok: true, requestId: action.requestId || genRequestId() };
      }
      case "set_mode":
        setMode(action.mode);
        return { ok: true };
      case "set_setting":
        setSettingValues(prev => ({ ...prev, [action.key]: action.value }));
        return { ok: true };
      case "relay_start":
        setTimeout(() => {
          setSimRelayActive(true);
          setSimRelayData({
            relayHostname: "k7mx2p.telemyapp.com",
            ingestUrl: "srtla://k7mx2p.telemyapp.com:5000",
            pairToken: "ABCD-1234-EFGH",
            region: "us-east-1",
            byorEnabled: true,
            byorRelayHost: "byor.telemy.test",
            byorRelayPort: 5000,
            byorStreamId: "live/custom",
          });
          setMode("irl");
        }, 1200);
        return { ok: true, requestId: action.requestId || genRequestId() };
      case "relay_stop":
        setSimRelayActive(false);
        setMode("studio");
        setSimRelayData((prev) => ({ ...prev }));
        return { ok: true };
      case "relay_connect_direct":
        setTimeout(() => {
          setSimRelayActive(true);
          setMode("irl");
        }, 400);
        return { ok: true, requestId: action.requestId || genRequestId() };
      case "relay_disconnect_direct":
        setSimRelayActive(false);
        setMode("studio");
        return { ok: true };
      case "save_config":
        setSimRelayData((prev) => ({
          ...prev,
          byorEnabled: action.byor_enabled != null ? !!action.byor_enabled : prev.byorEnabled,
          byorRelayHost: action.byor_relay_host != null ? String(action.byor_relay_host) : prev.byorRelayHost,
          byorRelayPort: action.byor_relay_port != null ? Number(action.byor_relay_port) || 5000 : prev.byorRelayPort,
          byorStreamId: action.byor_stream_id != null ? String(action.byor_stream_id) : prev.byorStreamId,
          relayHostname: action.byor_relay_host != null ? String(action.byor_relay_host) : prev.relayHostname,
          ingestUrl: action.byor_relay_host != null
            ? `srtla://${String(action.byor_relay_host)}:${action.byor_relay_port != null ? (Number(action.byor_relay_port) || 5000) : (prev.byorRelayPort || 5000)}`
            : prev.ingestUrl,
        }));
        return { ok: true };
      case "request_status":
        return { ok: true };
      default:
        return { ok: false, error: "unsupported_action_type" };
    }
  }, []);

  return { state, sendAction };
}
