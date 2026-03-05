import { useState, useEffect, useCallback, useMemo } from "react";
import { SIM_SCENES, SIM_SETTING_DEFS, SIM_EVENTS, OBS_YAMI_GREY_DEFAULTS, ENGINE_STATES, DEFAULT_AUTO_SCENE_RULES } from "./constants.js";
import { genRequestId } from "./utils.js";

// ---------------------------------------------------------------------------
// Simulation layer — produces exact bridge getState() shape
// ---------------------------------------------------------------------------
export function useSimulatedState() {
  const [mode, setMode] = useState("irl");
  const [simRelayActive, setSimRelayActive] = useState(true);
  const [simRelayData, setSimRelayData] = useState({
    ingestUrl: "srtla://203.0.113.42:5000",
    pairToken: "ABCD-1234-EFGH",
    region: "us-east-1",
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
      version: "v0.0.3",
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
      region: simRelayActive ? (simRelayData.region || "us-east-1") : "us-east-1",
      latencyMs: simRelayActive ? 42 : null,
      uptimeSec: simRelayActive ? elapsed : 0,
      graceRemainingSeconds: null,
      ingestUrl: simRelayActive ? (simRelayData.ingestUrl || null) : null,
      pairToken: simRelayActive ? (simRelayData.pairToken || null) : null,
      wsUrl: null,
      graceWindowSeconds: null,
      maxSessionSeconds: null,
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
            ingestUrl: "srtla://203.0.113.42:5000",
            pairToken: "ABCD-1234-EFGH",
            region: "us-east-1",
          });
        }, 1200);
        return { ok: true, requestId: action.requestId || genRequestId() };
      case "relay_stop":
        setSimRelayActive(false);
        setSimRelayData({});
        return { ok: true };
      case "request_status":
        return { ok: true };
      default:
        return { ok: false, error: "unsupported_action_type" };
    }
  }, []);

  return { state, sendAction };
}
