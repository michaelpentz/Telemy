import { useState, useEffect, useRef, useCallback, useMemo } from "react";

// =============================================================================
// AEGIS DOCK v0.0.3 — OBS Plugin Dock (Narrow / 320px)
// =============================================================================
//
// Contract-aligned to real bridge shape from aegis-dock-bridge.js.
// State is read from window.aegisDockNative.getState() which returns the
// exact nested structure documented in AGENT_RELAY.md.
//
// Data source legend:
//   [IPC]         — from status_snapshot via named pipe IPC v1
//   [PLUGIN]      — from plugin-local OBS API callbacks
//   [BRIDGE]      — derived/projected by aegis-dock-bridge.js
//   [PLACEHOLDER] — not yet in IPC v1; simulated for now
//   [CONFIG]      — plugin-local config, not in IPC scope
//
// Runtime contract:
//   Read:    window.aegisDockNative.getState()      -> DockState
//   Caps:    window.aegisDockNative.getCapabilities()
//   Action:  window.aegisDockNative.sendDockAction({ type, ...payload })
//   Result:  window.aegisDockNative.receiveDockActionResultJson(json)
//            -> dispatched as CustomEvent "aegis:dock:action-native-result"
//
// =============================================================================


// =============================================================================
// CONSTANTS & THEME
// =============================================================================

// Canonical engine states (STATE_MACHINE_v1.md)
const ENGINE_STATES = [
  { id: "STUDIO",         short: "STU",  color: "#5ba3f5", bgActive: "#0d1a2e", borderActive: "#1a3a5a" },
  { id: "IRL_CONNECTING",  short: "CONN", color: "#fbbf24", bgActive: "#261e0d", borderActive: "#3a2a0d" },
  { id: "IRL_ACTIVE",      short: "ACTV", color: "#4ade80", bgActive: "#0d2618", borderActive: "#1a3a1a" },
  { id: "IRL_GRACE",       short: "GRC",  color: "#a78bfa", bgActive: "#1a0d26", borderActive: "#2a1a3a" },
  { id: "DEGRADED",        short: "DRGD", color: "#fbbf24", bgActive: "#261e0d", borderActive: "#3a2a0d" },
  { id: "FATAL",           short: "FATL", color: "#da3633", bgActive: "#260d0d", borderActive: "#3a1a1a" },
];

const HEALTH_COLORS = {
  healthy:  "#2ea043",
  degraded: "#d29922",
  offline:  "#da3633",
};

const SCENE_INTENT_COLORS = {
  LIVE:    { bg: "#1a3a1a", border: "#2ea043", text: "#4ade80" },
  BRB:     { bg: "#2a1a2a", border: "#8b5cf6", text: "#a78bfa" },
  HOLD:    { bg: "#3a2a1a", border: "#d29922", text: "#fbbf24" },
  OFFLINE: { bg: "#1a1a2a", border: "#4a4f5c", text: "#8b8f98" },
};

const PIPE_STATUS_COLORS = {
  ok:       "#2ea043",
  degraded: "#d29922",
  down:     "#da3633",
};

// Per-key colors for settings toggles (bridge doesn't provide colors)
const SETTING_COLORS = {
  auto_scene_switch:   "#2ea043",
  low_quality_fallback: "#d29922",
  manual_override:     "#5ba3f5",
  chat_bot:            "#8b8f98",
  alerts:              "#2d7aed",
};

// UI anti-mash timing (tuned for OBS dock responsiveness + IPC round-trip cadence)
const UI_ACTION_COOLDOWNS_MS = {
  switchScene: 500,
  setMode: 500,
  setSetting: 350,
  autoSceneSwitch: 500,
};
const AUTO_SWITCH_LOCK_TIMEOUT_MS = 1500;

const OUTPUT_HEALTH_COLORS = {
  healthy:  "#2ea043",
  good:     "#8ac926",
  warning:  "#d29922",
  degraded: "#e85d04",
  critical: "#da3633",
};

function getOutputHealthColor(currentKbps, maxObservedKbps) {
  if (!maxObservedKbps || maxObservedKbps <= 0 || !currentKbps) return OUTPUT_HEALTH_COLORS.critical;
  const pct = currentKbps / maxObservedKbps;
  if (pct >= 0.9) return OUTPUT_HEALTH_COLORS.healthy;
  if (pct >= 0.7) return OUTPUT_HEALTH_COLORS.good;
  if (pct >= 0.5) return OUTPUT_HEALTH_COLORS.warning;
  if (pct >= 0.3) return OUTPUT_HEALTH_COLORS.degraded;
  return OUTPUT_HEALTH_COLORS.critical;
}

const SCENE_LINK_STORAGE_KEY = "aegis.scene.intent.links.v1";
const SCENE_LINK_NAME_STORAGE_KEY = "aegis.scene.intent.links.by_name.v1";
const AUTO_SCENE_RULES_STORAGE_KEY = "aegis.auto.scene.rules.v2";
// AUTO_SWITCH_SOURCE_OPTIONS removed — auto-switch source now derived from relay.active
const DEFAULT_AUTO_SCENE_RULES = [
  { id: "live_main", label: "Live - Main", intent: "LIVE", thresholdEnabled: false, thresholdMbps: null, isDefault: true, bgColor: "#2ea043" },
  { id: "low_bitrate_fallback", label: "Low Bitrate Fallback", intent: "HOLD", thresholdEnabled: true, thresholdMbps: 1.0, isDefault: false, bgColor: "#d29922" },
  { id: "brb_reconnecting", label: "BRB - Reconnecting", intent: "BRB", thresholdEnabled: true, thresholdMbps: 0.2, isDefault: false, bgColor: "#8b5cf6" },
  { id: "starting_soon", label: "Starting Soon", intent: "OFFLINE", thresholdEnabled: false, thresholdMbps: null, isDefault: false, bgColor: "#8b8f98" },
  { id: "ending", label: "Ending", intent: "OFFLINE", thresholdEnabled: false, thresholdMbps: null, isDefault: false, bgColor: "#8b8f98" },
];

const RULE_BG_PRESETS = [
  { id: "live", color: "#2ea043", label: "Live Green" },
  { id: "hold", color: "#d29922", label: "Hold Amber" },
  { id: "brb", color: "#8b5cf6", label: "BRB Violet" },
  { id: "offline", color: "#8b8f98", label: "Offline Slate" },
  { id: "alert", color: "#da3633", label: "Alert Red" },
];

const SCENE_PROFILE_NAME_HINTS = {
  live_main: ["main", "live - main", "live main", "live"],
  low_bitrate_fallback: ["low bitrate default scene", "low bitrate fallback", "low bitrate", "fallback", "low", "test"],
  brb_reconnecting: ["brb", "brb - reconnecting", "brb reconnecting", "reconnecting"],
  starting_soon: ["game audio", "starting soon", "starting"],
  ending: ["game audio", "ending", "end"],
};

// =============================================================================
// THEME DEFAULTS — Yami Grey palette (overridden by state.theme at runtime)
// =============================================================================

const OBS_YAMI_GREY_DEFAULTS = {
  bg:        "#1e1e1e",  // QPalette::Window — main dock background
  surface:   "#272727",  // QPalette::Base — section headers, card backgrounds
  panel:     "#2d2d2d",  // QPalette::Button — slightly elevated panels
  text:      "#e0e0e0",  // QPalette::WindowText — primary text
  textMuted: "#909090",  // ~60% text — secondary/dim labels
  accent:    "#1473e6",  // QPalette::Highlight — OBS brand blue
  border:    "#383838",  // derived — dividers, card outlines
  scrollbar: "#484848",  // derived — scrollbar thumb
  fontFamily: "",
};

// Sim scene definitions matching bridge shape
const SIM_SCENES = [
  { id: "scene_1", name: "Live - Main",           kind: null, intent: "live", index: 0 },
  { id: "scene_2", name: "Low Bitrate Fallback",   kind: null, intent: "hold", index: 1 },
  { id: "scene_3", name: "BRB - Reconnecting",     kind: null, intent: "brb",  index: 2 },
  { id: "scene_4", name: "Starting Soon",           kind: null, intent: null,   index: 3 },
  { id: "scene_5", name: "Ending",                  kind: null, intent: null,   index: 4 },
];

const SIM_SETTING_DEFS = [
  { key: "auto_scene_switch",   label: "Auto Scene Switch" },
  { key: "low_quality_fallback", label: "Low Bitrate Failover" },
  { key: "manual_override",     label: "Manual Override" },
  { key: "chat_bot",            label: "Chat Bot Integration" },
  { key: "alerts",              label: "Alert on Disconnect" },
];

const SIM_EVENTS = [
  { id: "e1", time: "01:04:07", tsUnixMs: Date.now() - 60000,  type: "success", msg: "Bitrate recovered \u2192 IRL_ACTIVE", source: "bridge" },
  { id: "e2", time: "01:03:42", tsUnixMs: Date.now() - 85000,  type: "warning", msg: "Low bitrate detected (intent BRB)",   source: "bridge" },
  { id: "e3", time: "01:03:40", tsUnixMs: Date.now() - 87000,  type: "warning", msg: "SIM 1 signal degraded",               source: "bridge" },
  { id: "e4", time: "00:58:12", tsUnixMs: Date.now() - 420000, type: "info",    msg: "Relay telemetry connected",           source: "ipc" },
  { id: "e5", time: "00:00:01", tsUnixMs: Date.now() - 3900000, type: "info",   msg: "Core connected",                      source: "ipc" },
];


// =============================================================================
// CSS
// =============================================================================

function getDockCss(theme) {
  return `
  /* === OBS CEF host sizing — ensures height:100% propagates to the component === */
  html, body { height: 100%; margin: 0; padding: 0; overflow: hidden; }
  #root { height: 100%; }

  @keyframes pulse { 0%, 100% { transform: scale(1); opacity: 0.3; } 50% { transform: scale(2.2); opacity: 0; } }
  @keyframes slideIn { from { opacity: 0; transform: translateY(-4px); } to { opacity: 1; transform: translateY(0); } }
  @keyframes shimmer {
    0% { background-position: -200% 0; }
    100% { background-position: 200% 0; }
  }
  @keyframes railPulse {
    0%, 100% { opacity: 0.7; }
    50% { opacity: 1; }
  }
  .aegis-dock-scroll::-webkit-scrollbar { width: 3px; }
  .aegis-dock-scroll::-webkit-scrollbar-track { background: transparent; }
  .aegis-dock-scroll::-webkit-scrollbar-thumb { background: ${theme.scrollbar}; border-radius: 2px; }
  .aegis-dock-scroll::-webkit-scrollbar-thumb:hover { background: ${theme.border}; border-radius: 2px; }
`;
}


// =============================================================================
// UTILITIES
// =============================================================================

let _reqCounter = 0;
function genRequestId() {
  return `dock_${Date.now()}_${++_reqCounter}`;
}

function formatTime(s) {
  if (s == null || s < 0) return "\u2014";
  const sec = Math.floor(s);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const ss = sec % 60;
  return `${h.toString().padStart(2, "0")}:${m.toString().padStart(2, "0")}:${ss.toString().padStart(2, "0")}`;
}

function parseHexColor(hex) {
  const raw = String(hex || "").trim();
  const cleaned = raw.startsWith("#") ? raw.slice(1) : raw;
  if (!/^[0-9a-fA-F]{3}$|^[0-9a-fA-F]{6}$/.test(cleaned)) return null;
  const normalized = cleaned.length === 3
    ? cleaned.split("").map((c) => c + c).join("")
    : cleaned;
  return {
    r: parseInt(normalized.slice(0, 2), 16),
    g: parseInt(normalized.slice(2, 4), 16),
    b: parseInt(normalized.slice(4, 6), 16),
  };
}

function toRgba(hex, alpha) {
  const rgb = parseHexColor(hex);
  if (!rgb) return `rgba(91,163,245,${alpha})`;
  return `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${alpha})`;
}

function isLightColor(hex) {
  const rgb = parseHexColor(hex);
  if (!rgb) return false;
  const luminance = (0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b) / 255;
  return luminance > 0.6;
}

function normalizeOptionalHexColor(value) {
  const rgb = parseHexColor(value);
  if (!rgb) return null;
  const toHex = (n) => n.toString(16).padStart(2, "0");
  return `#${toHex(rgb.r)}${toHex(rgb.g)}${toHex(rgb.b)}`;
}

function getDefaultRuleBgColor(rule) {
  const id = String(rule?.id || "");
  const intent = String(rule?.intent || "").toUpperCase();
  if (id === "live_main" || intent === "LIVE") return "#2ea043";
  if (id === "low_bitrate_fallback" || intent === "HOLD") return "#d29922";
  if (id === "brb_reconnecting" || intent === "BRB") return "#8b5cf6";
  if (id === "starting_soon" || id === "ending" || intent === "OFFLINE") return "#8b8f98";
  return "#8b8f98";
}

function normalizeIntent(intent) {
  if (!intent) return null;
  const upper = intent.toUpperCase();
  if (upper in SCENE_INTENT_COLORS) return upper;
  return null;
}

function inferIntentFromName(name) {
  if (!name) return "OFFLINE";
  const lower = name.toLowerCase();
  if (lower.includes("live") || lower.includes("main")) return "LIVE";
  if (lower.includes("brb") || lower.includes("reconnect")) return "BRB";
  if (lower.includes("low") || lower.includes("fallback")) return "HOLD";
  return "OFFLINE";
}

function normalizeSceneName(name) {
  return String(name || "").toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();
}

function findBestSceneIdForRule(rule, sceneItems) {
  const hints = SCENE_PROFILE_NAME_HINTS[rule.id] || [rule.label];
  if (!Array.isArray(sceneItems) || sceneItems.length === 0) return "";
  const normalizedHints = hints.map(normalizeSceneName).filter(Boolean);
  for (const scene of sceneItems) {
    const n = normalizeSceneName(scene.name);
    if (normalizedHints.includes(n)) return scene.id || "";
  }
  for (const scene of sceneItems) {
    const n = normalizeSceneName(scene.name);
    if (normalizedHints.some((h) => h && n.includes(h))) return scene.id || "";
  }
  return "";
}

function mapRelayStatusForUi(status) {
  const raw = (status || "").toLowerCase();
  if (raw === "provisioning") return "connecting";
  return raw || "inactive";
}

function loadSceneIntentLinks() {
  if (typeof window === "undefined") {
    return {};
  }
  try {
    const storage = window.localStorage;
    if (!storage) return {};
    const raw = storage.getItem(SCENE_LINK_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : {};
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch (_) {
    return {};
  }
}

function loadSceneIntentLinkNames() {
  if (typeof window === "undefined") {
    return {};
  }
  try {
    const storage = window.localStorage;
    if (!storage) return {};
    const raw = storage.getItem(SCENE_LINK_NAME_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : {};
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch (_) {
    return {};
  }
}

function findSceneIdByName(sceneName, sceneItems) {
  const target = normalizeSceneName(sceneName);
  if (!target || !Array.isArray(sceneItems) || sceneItems.length === 0) return "";
  for (const scene of sceneItems) {
    if (normalizeSceneName(scene?.name) === target) return String(scene.id || "");
  }
  for (const scene of sceneItems) {
    const normalized = normalizeSceneName(scene?.name);
    if (normalized.includes(target) || target.includes(normalized)) return String(scene.id || "");
  }
  return "";
}

function normalizeLinkMap(raw) {
  if (!raw || typeof raw !== "object") return {};
  const out = {};
  Object.keys(raw).forEach((k) => {
    out[String(k)] = String(raw[k] || "");
  });
  return out;
}

// loadAutoSwitchSource removed — auto-switch source now derived from relay.active

function loadAutoSceneRules() {
  if (typeof window === "undefined") return DEFAULT_AUTO_SCENE_RULES;
  try {
    const storage = window.localStorage;
    if (!storage) return DEFAULT_AUTO_SCENE_RULES;
    const raw = storage.getItem(AUTO_SCENE_RULES_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : null;
    return normalizeAutoSceneRulesValue(parsed);
  } catch (_) {
    return DEFAULT_AUTO_SCENE_RULES;
  }
}

function normalizeAutoSceneRulesValue(rules) {
  if (!Array.isArray(rules) || rules.length === 0) return DEFAULT_AUTO_SCENE_RULES;
  const normalized = rules
    .map((r, idx) => {
      if (!r || typeof r !== "object") return null;
      const id = String(r.id || `rule_${idx}`);
      const label = String(r.label || `Rule ${idx + 1}`).slice(0, 40);
      const intent = normalizeIntent(r.intent) || "HOLD";
      const threshold = (r.thresholdMbps === "" || r.thresholdMbps == null)
        ? null
        : Number(r.thresholdMbps);
      const thresholdEnabled = typeof r.thresholdEnabled === "boolean"
        ? r.thresholdEnabled
        : (Number.isFinite(threshold) && threshold >= 0);
      return {
        id,
        label,
        intent,
        thresholdEnabled,
        thresholdMbps: Number.isFinite(threshold) && threshold >= 0 ? threshold : null,
        isDefault: !!r.isDefault,
        bgColor: normalizeOptionalHexColor(r.bgColor) || getDefaultRuleBgColor({ id, intent }),
      };
    })
    .filter(Boolean);
  return normalized.length ? normalized : DEFAULT_AUTO_SCENE_RULES;
}


// =============================================================================
// HOOKS
// =============================================================================

function useAnimatedValue(target, duration = 800) {
  const [value, setValue] = useState(target);
  const ref = useRef(target);
  useEffect(() => {
    const start = ref.current;
    const diff = target - start;
    if (Math.abs(diff) < 0.5) { ref.current = target; setValue(target); return; }
    const startTime = performance.now();
    let raf;
    const step = (now) => {
      const elapsed = now - startTime;
      const progress = Math.min(elapsed / duration, 1);
      const eased = 1 - Math.pow(1 - progress, 3);
      setValue(start + diff * eased);
      if (progress < 1) raf = requestAnimationFrame(step);
      else ref.current = target;
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, [target, duration]);
  return value;
}

function useRollingMaxBitrate(outputItems) {
  const maxMapRef = useRef({});
  const resultRef = useRef({ maxMap: {}, sectionMax: 0 });
  // Compute inline during render — intentional ref mutation for rolling tracker.
  // No setState to avoid re-render loops when outputItems is a fresh array each render.
  const maxMap = maxMapRef.current;
  let sectionMax = 0;
  if (Array.isArray(outputItems)) {
    outputItems.forEach(item => {
      const key = item.id || item.name || item.platform;
      if (key && item.kbps > 0) {
        const prev = maxMap[key] || 0;
        const decayed = prev > 0 ? prev * 0.998 : 0;
        maxMap[key] = Math.max(decayed, item.kbps);
      }
      if (key && maxMap[key] > sectionMax) sectionMax = maxMap[key];
    });
  }
  resultRef.current = { maxMap, sectionMax };
  return resultRef.current;
}

function useDockCompactMode(containerRef) {
  const [mode, setMode] = useState("regular");

  useEffect(() => {
    const el = containerRef.current;
    if (!el || typeof ResizeObserver === "undefined") return;

    const applyMode = (width) => {
      if (width <= 280) {
        setMode("ultra");
      } else if (width <= 340) {
        setMode("compact");
      } else {
        setMode("regular");
      }
    };

    applyMode(el.clientWidth || 0);
    const ro = new ResizeObserver((entries) => {
      const width = entries?.[0]?.contentRect?.width || el.clientWidth || 0;
      applyMode(width);
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, [containerRef]);

  return mode;
}


// ---------------------------------------------------------------------------
// Bridge integration hook
// ---------------------------------------------------------------------------
// Reads DockState from the native bridge when available. Listens for bridge
// events + native action results. Falls back gracefully when bridge absent.
//
function useDockState() {
  const [state, setState] = useState(null);
  const [bridgeAvailable, setBridgeAvailable] = useState(false);
  const actionMapRef = useRef({});

  useEffect(() => {
    let didInit = false;
    let unsub;
    let poll;
    let fastPoll;
    let earlyRefresh;
    let statusRequest;

    const stateEvents = [
      "aegis:dock:native-ready",
      "aegis:dock:host-fallback",
      "aegis:dock:ipc-envelope-json",
      "aegis:dock:scene-snapshot",
      "aegis:dock:scene-snapshot-json",
      "aegis:dock:current-scene",
      "aegis:dock:pipe-status",
      "aegis:dock:scene-switch-completed",
      "aegis:dock:action-native-result",
    ];

    let refresh = () => {};
    const initNativeBridge = () => {
      if (didInit) return true;
      const native = window.aegisDockNative;
      if (!native || typeof native.getState !== "function") return false;

      didInit = true;
      setBridgeAvailable(true);

      refresh = () => {
        try { setState(native.getState()); } catch (_) {}
      };

      try { setState(native.getState()); } catch (_) {}
      stateEvents.forEach(e => window.addEventListener(e, refresh));

      const host = window.aegisDockHost;
      if (host && typeof host.subscribe === "function") {
        unsub = host.subscribe(() => refresh());
      }

      poll = setInterval(refresh, 2000);
      fastPoll = setInterval(refresh, 250);
      setTimeout(() => {
        if (fastPoll) {
          clearInterval(fastPoll);
          fastPoll = null;
        }
      }, 6000);
      earlyRefresh = setTimeout(() => {
        try { setState(native.getState()); } catch (_) {}
      }, 150);
      statusRequest = setTimeout(() => {
        try {
          native.sendDockAction({ type: "request_status", requestId: genRequestId() });
        } catch (_) {}
      }, 400);
      return true;
    };

    initNativeBridge();
    const retry = setInterval(() => {
      if (initNativeBridge()) clearInterval(retry);
    }, 250);

    return () => {
      clearInterval(retry);
      stateEvents.forEach(e => window.removeEventListener(e, refresh));
      if (unsub) unsub();
      if (poll) clearInterval(poll);
      if (fastPoll) clearInterval(fastPoll);
      if (earlyRefresh) clearTimeout(earlyRefresh);
      if (statusRequest) clearTimeout(statusRequest);
    };
  }, []);

  // Native action result tracking
  useEffect(() => {
    const handler = (e) => {
      const result = e.detail;
      if (!result?.requestId) return;
      const entry = actionMapRef.current[result.requestId];
      if (entry) {
        entry.status = result.status;
        entry.ok = result.ok;
        entry.error = result.error;
        if (result.status === "completed" || result.status === "failed" || result.status === "rejected") {
          setTimeout(() => { delete actionMapRef.current[result.requestId]; }, 3000);
        }
      }
    };
    window.addEventListener("aegis:dock:action-native-result", handler);
    return () => window.removeEventListener("aegis:dock:action-native-result", handler);
  }, []);

  const sendAction = useCallback((action) => {
    const native = window.aegisDockNative;
    if (!native || typeof native.sendDockAction !== "function") {
      console.log("[aegis-dock] sendDockAction (no native):", action);
      return null;
    }
    // Ensure requestId
    if (!action.requestId) action.requestId = genRequestId();
    // Track in-flight
    actionMapRef.current[action.requestId] = {
      type: action.type, status: "optimistic", ts: Date.now()
    };
    const result = native.sendDockAction(action);
    // Re-read state immediately (bridge may have mutated locally)
    try { setState(native.getState()); } catch (_) {}
    return result;
  }, []);

  return { state, sendAction, bridgeAvailable };
}


// ---------------------------------------------------------------------------
// Simulation layer — produces exact bridge getState() shape
// ---------------------------------------------------------------------------
function useSimulatedState() {
  const [mode, setMode] = useState("irl");
  const [simRelayActive, setSimRelayActive] = useState(true);
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
    outputs: {
      groups: [
        {
          name: "Horizontal",
          encoder: "x264",
          resolution: "1920x1080",
          totalBitrateKbps: Math.floor((sim1 + sim2) * 0.75),
          avgLagMs: 2.1,
          items: [
            { id: "twitch", name: "Twitch", platform: "Twitch", kbps: Math.max(800, Math.floor((sim1 + sim2) * 0.35)), fps: 60, dropPct: 0.01, active: true },
            { id: "kick", name: "Kick", platform: "Kick", kbps: Math.max(600, Math.floor((sim1 + sim2) * 0.22)), fps: 60, dropPct: 0.02, active: simRelayActive },
            { id: "yt_horiz", name: "YT Horizontal", platform: "YouTube", kbps: Math.max(600, Math.floor((sim1 + sim2) * 0.18)), fps: 60, dropPct: 0.01, active: true },
          ],
        },
        {
          name: "Vertical",
          encoder: "x264",
          resolution: "1080x1920",
          totalBitrateKbps: Math.floor((sim1 + sim2) * 0.25),
          avgLagMs: 3.0,
          items: [
            { id: "tiktok", name: "TikTok", platform: "TikTok", kbps: Math.max(300, Math.floor((sim1 + sim2) * 0.13)), fps: 30, dropPct: 0.03, active: true },
            { id: "yt_shorts", name: "YT Shorts", platform: "YT Shorts", kbps: Math.max(300, Math.floor((sim1 + sim2) * 0.12)), fps: 30, dropPct: 0.02, active: true },
          ],
        },
      ],
      hidden: [
        { id: "virtualcam", name: "Virtual Camera", active: false },
        { id: "recording", name: "Recording", active: false },
      ],
    },
    relay: {
      licensed: true,
      active: simRelayActive,
      enabled: simRelayActive, // backward compat
      status: simRelayActive ? "active" : "inactive",
      region: "us-east-1",
      latencyMs: simRelayActive ? 42 : null,
      uptimeSec: simRelayActive ? elapsed : 0,
      graceRemainingSeconds: null,
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
  }), [mode, elapsed, activeSceneId, pendingSceneId, sim1, sim2, settingValues, simRelayActive]);

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
        setTimeout(() => setSimRelayActive(true), 1200);
        return { ok: true, requestId: action.requestId || genRequestId() };
      case "relay_stop":
        setSimRelayActive(false);
        return { ok: true };
      case "request_status":
        return { ok: true };
      default:
        return { ok: false, error: "unsupported_action_type" };
    }
  }, []);

  return { state, sendAction };
}


// =============================================================================
// UI COMPONENTS
// =============================================================================

// --- Collapsible Section ---
function Section({ title, icon, badge, badgeColor, defaultOpen = false, compact = false, children }) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <div style={{
      borderBottom: "1px solid var(--theme-border, #1a1d23)",
      background: open ? "rgba(128,128,128,0.04)" : "transparent",
      transition: "background 0.2s ease",
    }}>
      <button
        onClick={() => setOpen(!open)}
        style={{
          width: "100%", display: "flex", alignItems: "center", gap: 8,
          padding: compact ? "9px 10px" : "10px 12px", border: "none", background: "none",
          color: "var(--theme-text-muted, #c8ccd4)", cursor: "pointer", fontSize: compact ? 10 : 11,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
          fontWeight: 600, letterSpacing: "0.08em", textTransform: "uppercase",
          transition: "color 0.15s ease",
        }}
        onMouseEnter={e => e.currentTarget.style.color = "var(--theme-text, #fff)"}
        onMouseLeave={e => e.currentTarget.style.color = "var(--theme-text-muted, #c8ccd4)"}
      >
        <span style={{ fontSize: 13, opacity: 0.6, lineHeight: 1 }}>{icon}</span>
        <span style={{ flex: 1, textAlign: "left" }}>{title}</span>
        {badge != null && (
          <span style={{
            background: badgeColor || "#2d7aed",
            color: "var(--theme-bg, #fff)", fontSize: compact ? 7 : 8, fontWeight: 700,
            padding: compact ? "1px 4px" : "2px 6px", borderRadius: 3, letterSpacing: "0.04em",
            fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
            maxWidth: compact ? 64 : 92,
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap",
          }}>{badge}</span>
        )}
        <svg width="10" height="10" viewBox="0 0 10 10"
          style={{
            transform: open ? "rotate(0deg)" : "rotate(-90deg)",
            transition: "transform 0.2s ease", opacity: 0.4,
          }}>
          <path d="M2 3.5L5 6.5L8 3.5" stroke="currentColor" strokeWidth="1.5"
            fill="none" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </button>
      <div style={{
        maxHeight: open ? 800 : 0,
        overflow: "hidden",
        transition: "max-height 0.3s cubic-bezier(0.4,0,0.2,1)",
        opacity: open ? 1 : 0,
      }}>
        <div style={{ padding: compact ? "2px 10px 10px" : "2px 12px 12px" }}>
          {children}
        </div>
      </div>
    </div>
  );
}

// --- Status Dot ---
function StatusDot({ color, pulse }) {
  return (
    <span style={{ position: "relative", display: "inline-flex", alignItems: "center", justifyContent: "center" }}>
      {pulse && (
        <span style={{
          position: "absolute", width: 10, height: 10, borderRadius: "50%",
          background: color, opacity: 0.3,
          animation: "pulse 2s ease-in-out infinite",
        }} />
      )}
      <span style={{
        width: 6, height: 6, borderRadius: "50%", background: color,
        boxShadow: `0 0 6px ${color}40`,
      }} />
    </span>
  );
}

// --- Bitrate Bar ---
function BitrateBar({ value, max, color, label }) {
  const pct = max > 0 ? Math.min((value / max) * 100, 100) : 0;
  return (
    <div style={{ marginBottom: 6 }}>
      <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 3 }}>
        <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)" }}>{label}</span>
        <span style={{ fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)", fontWeight: 600 }}>
          {(value / 1000).toFixed(1)} Mbps
        </span>
      </div>
      <div style={{ height: 4, background: "var(--theme-surface, #1a1d23)", borderRadius: 2, overflow: "hidden" }}>
        <div style={{
          height: "100%", width: `${pct}%`,
          background: `linear-gradient(90deg, ${color}, ${color}cc)`,
          borderRadius: 2, transition: "width 0.6s cubic-bezier(0.4,0,0.2,1)",
          boxShadow: `0 0 8px ${color}30`,
        }} />
      </div>
    </div>
  );
}

// --- Output Bar ---
// Renders a single encoder output as a labelled bitrate bar with health color.
function OutputBar({ name, bitrateKbps, fps, dropPct, active, maxBitrate, compact = false }) {
  const healthColor = getOutputHealthColor(bitrateKbps, maxBitrate);
  const animBitrate = useAnimatedValue(active ? (bitrateKbps || 0) : 0, 600);
  const pct = maxBitrate > 0 ? Math.min((animBitrate / maxBitrate) * 100, 100) : 0;
  const inactive = !active;

  return (
    <div style={{ marginBottom: compact ? 6 : 8, opacity: inactive ? 0.4 : 1, transition: "opacity 0.3s ease" }}>
      <div style={{
        display: "flex", justifyContent: "space-between", alignItems: "baseline",
        marginBottom: 3, fontSize: compact ? 9 : 10,
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
      }}>
        <span style={{
          color: "var(--theme-text, #e0e2e8)", fontWeight: 600,
          overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", flex: 1, marginRight: 8,
        }}>
          {name || "Output"}
        </span>
        <span style={{ color: "var(--theme-text-muted, #8b8f98)", flexShrink: 0, whiteSpace: "nowrap" }}>
          {inactive ? "\u2014" : (
            <>
              {bitrateKbps != null ? `${(bitrateKbps / 1000).toFixed(1)} Mbps` : "\u2014"}
              {"  "}
              {fps != null ? `${Math.round(fps)}fps` : ""}
              {"  "}
              {dropPct != null ? `${dropPct.toFixed(2)}%` : ""}
            </>
          )}
        </span>
      </div>
      <div style={{
        height: compact ? 3 : 4, background: "var(--theme-border, #2a2d35)",
        borderRadius: 2, overflow: "hidden",
      }}>
        <div style={{
          height: "100%", width: `${pct}%`,
          background: healthColor,
          borderRadius: 2,
          transition: "width 0.6s cubic-bezier(0.4,0,0.2,1), background 0.4s ease",
          boxShadow: inactive ? "none" : `0 0 4px ${healthColor}30`,
        }} />
      </div>
    </div>
  );
}

// --- Encoder Group Header ---
// Divider row labelling a named encoder pool with aggregate bitrate + lag.
function EncoderGroupHeader({ name, resolution, totalBitrateKbps, avgLagMs, compact = false }) {
  if (name === "Ungrouped") return null;
  return (
    <div style={{ marginTop: 8, marginBottom: 6 }}>
      <div style={{
        display: "flex", alignItems: "center", gap: 6,
        fontSize: compact ? 8 : 9,
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
        color: "var(--theme-text-muted, #8b8f98)",
        letterSpacing: "0.05em", fontWeight: 700,
      }}>
        <div style={{ flex: 1, height: 1, background: "var(--theme-border, #2a2d35)" }} />
        <span>{name.toUpperCase()}</span>
        {resolution && (
          <span style={{
            fontSize: compact ? 7 : 8, padding: "1px 4px", borderRadius: 2,
            background: "var(--theme-surface, #13151a)", border: "1px solid var(--theme-border, #2a2d35)",
          }}>
            {resolution}
          </span>
        )}
        <div style={{ flex: 1, height: 1, background: "var(--theme-border, #2a2d35)" }} />
      </div>
      <div style={{
        fontSize: compact ? 8 : 9, color: "var(--theme-text-muted, #6b7080)",
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
        textAlign: "center", marginTop: 3,
      }}>
        Pool {totalBitrateKbps != null ? `${(totalBitrateKbps / 1000).toFixed(1)} Mbps` : "\u2014"}
        {"  \u2022  "}
        Lag {avgLagMs != null ? `${avgLagMs.toFixed(1)}ms` : "\u2014"}
      </div>
    </div>
  );
}

// --- Hidden Outputs Toggle ---
// Collapsible list of outputs that are hidden from the main section.
function HiddenOutputsToggle({ items, compact = false }) {
  const [expanded, setExpanded] = useState(false);
  if (!items || items.length === 0) return null;
  return (
    <div style={{ marginTop: 6 }}>
      <div
        onClick={() => setExpanded(v => !v)}
        style={{
          display: "flex", alignItems: "center", gap: 6, cursor: "pointer",
          fontSize: compact ? 8 : 9,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
          color: "var(--theme-text-muted, #6b7080)",
        }}
      >
        <span>Hidden ({items.length})</span>
        <span style={{
          fontSize: compact ? 7 : 8, color: "var(--theme-accent, #5ba3f5)",
          textDecoration: "underline", textUnderlineOffset: 2,
        }}>
          {expanded ? "Hide" : "Show"}
        </span>
      </div>
      {expanded && (
        <div style={{ marginTop: 4, opacity: 0.5 }}>
          {items.map((item, idx) => (
            <div key={item.id || idx} style={{
              fontSize: compact ? 8 : 9, color: "var(--theme-text-muted, #6b7080)",
              fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
              marginBottom: 2,
            }}>
              {item.name || item.platform || `Output ${idx + 1}`}
              {item.active ? " (active)" : ""}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

// --- Scene Button ---
// [PLUGIN] scene list + active scene from OBS callbacks
// [BRIDGE] pendingSceneId tracked by bridge bookkeeping
function SceneButton({ name, active, pending, intent, compact = false, onClick }) {
  const c = SCENE_INTENT_COLORS[intent] || SCENE_INTENT_COLORS.OFFLINE;
  return (
    <button onClick={onClick} style={{
      width: "100%", padding: compact ? "6px 8px" : "7px 10px",
      border: `1px solid ${active ? c.border : pending ? "var(--theme-accent, #3a3d45)" : "var(--theme-border, #2a2d35)"}`,
      borderRadius: 4, background: active ? c.bg : "var(--theme-surface, #13151a)",
      display: "flex", alignItems: "center", gap: 8, cursor: "pointer",
      transition: "all 0.15s ease", marginBottom: 4,
      boxShadow: active ? `0 0 12px ${c.border}15, inset 0 1px 0 ${c.border}10` : "none",
      ...(pending && !active ? {
        backgroundImage: "linear-gradient(90deg, transparent 0%, rgba(91,163,245,0.04) 50%, transparent 100%)",
        backgroundSize: "200% 100%",
        animation: "shimmer 2s linear infinite",
      } : {}),
    }}
      onMouseEnter={e => { if (!active) e.currentTarget.style.borderColor = "var(--theme-accent, #3a3d45)"; }}
      onMouseLeave={e => { if (!active) e.currentTarget.style.borderColor = pending ? "var(--theme-accent, #3a3d45)" : "var(--theme-border, #2a2d35)"; }}
    >
      <span style={{
        width: 5, height: 5, borderRadius: "50%",
        background: active ? c.border : pending ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)",
        boxShadow: active ? `0 0 4px ${c.border}` : pending ? "0 0 4px var(--theme-accent, #5ba3f5)" : "none",
      }} />
      <span style={{
        fontSize: compact ? 10 : 11, fontWeight: active ? 600 : 400,
        color: active ? c.text : pending ? "var(--theme-accent, #5ba3f5)" : "var(--theme-text-muted, #8b8f98)",
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)", flex: 1, textAlign: "left", minWidth: 0,
        overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
      }}>{name}</span>
      {active && (
        <span style={{
          fontSize: compact ? 7 : 8, fontWeight: 700, color: c.border,
          background: `${c.border}15`, padding: compact ? "1px 4px" : "1px 5px",
          borderRadius: 2, textTransform: "uppercase", letterSpacing: "0.08em",
          flexShrink: 0,
        }}>{compact ? "ON" : "ACTIVE"}</span>
      )}
      {pending && !active && (
        <span style={{
          fontSize: compact ? 7 : 8, fontWeight: 700, color: "#5ba3f5",
          background: "#5ba3f515", padding: compact ? "1px 4px" : "1px 5px",
          borderRadius: 2, textTransform: "uppercase", letterSpacing: "0.08em",
          flexShrink: 0,
        }}>{compact ? "..." : "SWITCHING"}</span>
      )}
    </button>
  );
}

// --- Connection Card ---
// Uses normalized bridge fields: { name, type, signal, bitrate, status }
function ConnectionCard({ name, type, signal, bitrate, status, compact = false }) {
  const statusColors = { connected: "#2ea043", degraded: "#d29922", disconnected: "#da3633" };
  const col = statusColors[status] || statusColors.disconnected;
  const bars = [1, 2, 3, 4];
  return (
    <div style={{
      background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "8px 10px",
      border: `1px solid ${status === "connected" ? "var(--theme-accent, #1a3a1a)" : "var(--theme-border, #2a2d35)"}`,
      marginBottom: 4, transition: "border-color 0.2s ease",
    }}>
      <div style={{ display: "flex", alignItems: "center", gap: compact ? 4 : 6, marginBottom: 4 }}>
        <StatusDot color={col} pulse={status === "connected"} />
        <span style={{
          fontSize: compact ? 10 : 11, color: "var(--theme-text, #e0e2e8)", fontWeight: 600, flex: 1,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
          overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
        }}>{name}</span>
        <span style={{
          fontSize: compact ? 8 : 9, color: "var(--theme-text-muted, #6b7080)", fontWeight: 500,
          background: "var(--theme-surface, #1a1d23)", padding: compact ? "1px 4px" : "1px 5px", borderRadius: 2,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)", flexShrink: 0,
        }}>{type}</span>
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
        <div style={{ display: "flex", gap: 1.5, alignItems: "flex-end" }}>
          {bars.map(i => (
            <div key={i} style={{
              width: 3, height: 3 + i * 3, borderRadius: 1,
              background: i <= signal ? col : "var(--theme-border, #2a2d35)",
              transition: "background 0.3s ease",
              boxShadow: i <= signal ? `0 0 3px ${col}30` : "none",
            }} />
          ))}
        </div>
        <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)" }}>
          {bitrate > 0 ? `${(bitrate / 1000).toFixed(1)} Mbps` : "\u2014"}
        </span>
      </div>
    </div>
  );
}

// --- Engine State Chips (3x2 grid, canonical states from STATE_MACHINE_v1.md) ---
function EngineStateChips({ activeState, compact = false }) {
  return (
    <div style={{
      display: "grid", gridTemplateColumns: compact ? "1fr 1fr" : "1fr 1fr 1fr",
      gap: 3, marginBottom: 8,
    }}>
      {ENGINE_STATES.map((es) => {
        const isActive = activeState === es.id;
        return (
          <div key={es.id} style={{
            height: compact ? 20 : 22, borderRadius: 3, display: "flex",
            alignItems: "center", justifyContent: "center",
            fontSize: compact ? 6 : 7, fontWeight: 700, letterSpacing: "0.04em",
            fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
            background: isActive ? es.bgActive : "var(--theme-surface, #13151a)",
            border: `1px solid ${isActive ? es.borderActive : "var(--theme-border, #2a2d35)"}`,
            color: isActive ? es.color : "var(--theme-text-muted, #5a5f6d)",
            transition: "all 0.25s ease",
            boxShadow: isActive ? `0 0 8px ${es.color}15` : "none",
          }}>
            {es.short}
          </div>
        );
      })}
    </div>
  );
}

// --- Toggle Row (fully controlled — state comes from props) ---
function ToggleRow({ label, value, color, dimmed, onChange }) {
  const isOn = !!value;
  const isDimmed = dimmed || value === null;
  return (
    <div style={{
      display: "flex", alignItems: "center", justifyContent: "space-between",
      padding: "6px 0", borderBottom: "1px solid var(--theme-border, #13151a)",
      opacity: isDimmed ? 0.45 : 1,
    }}>
      <span style={{
        fontSize: 10, color: isOn ? "var(--theme-text, #c8ccd4)" : "var(--theme-text-muted, #5a5f6d)",
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
      }}>{label}</span>
      <button onClick={() => { if (!isDimmed && onChange) onChange(!isOn); }} style={{
        width: 32, height: 16, borderRadius: 8, border: "none",
        cursor: isDimmed ? "not-allowed" : "pointer",
        background: isOn ? (color || "var(--theme-accent, #2d7aed)") : "var(--theme-border, #2a2d35)",
        position: "relative", transition: "background 0.2s ease",
        flexShrink: 0,
      }}>
        <div style={{
          width: 12, height: 12, borderRadius: "50%", background: "var(--theme-text, #fff)",
          position: "absolute", top: 2,
          left: isOn ? 18 : 2,
          transition: "left 0.2s ease",
          boxShadow: "0 1px 3px rgba(0,0,0,0.3)",
        }} />
      </button>
    </div>
  );
}


// =============================================================================
// MAIN DOCK COMPONENT
// =============================================================================

export default function AegisDock() {
  const dockRootRef = useRef(null);
  const dockLayout = useDockCompactMode(dockRootRef);
  const isCompact = dockLayout === "compact" || dockLayout === "ultra";
  const isUltraCompact = dockLayout === "ultra";

  const bridge = useDockState();
  const sim = useSimulatedState();

  // Use bridge when available, otherwise simulation
  const useBridge = bridge.bridgeAvailable;
  const ds = useBridge ? (bridge.state || {}) : sim.state;
  const sendAction = useBridge ? bridge.sendAction : sim.sendAction;

  // --- Extract state slices (matching real bridge getState() shape) ---
  const header  = ds.header || {};
  const live    = ds.live || {};
  const scenes  = ds.scenes || {};
  const conns   = ds.connections?.items || [];
  const bitrate = ds.bitrate || {};
  const relay   = ds.relay || {};
  const failover = ds.failover || {};
  const settings = ds.settings?.items || [];
  const events  = ds.events || [];
  const pipe    = ds.pipe || {};
  const theme   = ds.theme || {};
  const settingsByKey = Object.fromEntries(settings.map(s => [s.key, s.value]));

  const mode = header.mode || "studio";
  const isLive = live.isLive || false;
  const elapsedSec = live.elapsedSec || 0;

  // Relay-derived state (replaces explicit mode gating)
  const relayActive = relay.active ?? relay.enabled ?? false;
  const relayLicensed = relay.licensed !== false; // default true for backward compat
  const derivedMode = relayActive && conns.length > 0 ? "irl" : "studio";

  // Relay activation UI state
  const [relayActivating, setRelayActivating] = useState(false);
  const [relayError, setRelayError] = useState(null);

  // Clear activating spinner when relay becomes active
  useEffect(() => {
    if (relayActive && relayActivating) {
      setRelayActivating(false);
      setRelayError(null);
    }
  }, [relayActive, relayActivating]);

  // Failover / health
  const engineState = failover.state || (relayActive ? "IRL_ACTIVE" : "STUDIO");
  const healthColor = HEALTH_COLORS[failover.health] || HEALTH_COLORS.offline;
  const healthLabel = (failover.health || "offline").toUpperCase();

  // Pipe status
  const pipeColor = PIPE_STATUS_COLORS[pipe.status] || PIPE_STATUS_COLORS.down;
  const pipeLabel = pipe.label || (pipe.status === "ok" ? "IPC: OK" : pipe.status === "degraded" ? "IPC: DEGRADED" : "IPC: DOWN");

  // Active scene (matched by ID)
  const allScenes = Array.isArray(scenes.items) ? scenes.items : [];
  const activeScene = allScenes.find((s) => s.id === scenes.activeSceneId) || null;
  const pendingScene = allScenes.find((s) => s.id === scenes.pendingSceneId) || null;
  const [autoSceneRules, setAutoSceneRules] = useState(() => loadAutoSceneRules());
  const [expandedRuleId, setExpandedRuleId] = useState(null);
  const [sceneIntentLinks, setSceneIntentLinks] = useState(() => loadSceneIntentLinks());
  const [sceneIntentLinkNames, setSceneIntentLinkNames] = useState(() => loadSceneIntentLinkNames());
  // autoSwitchSourceSelection removed — now derived from relayActive
  const [scenePanelExpanded, setScenePanelExpanded] = useState(false);
  const [scenePrefsHydrated, setScenePrefsHydrated] = useState(!useBridge);
  const scenePrefsSaveTimerRef = useRef(null);
  const resolveSceneIntent = useCallback((scene) => {
    if (!scene) return "OFFLINE";
    const matchedRule = autoSceneRules.find((rule) => sceneIntentLinks[rule.id] === scene.id);
    if (matchedRule) return matchedRule.intent;
    return normalizeIntent(scene.intent) || inferIntentFromName(scene.name);
  }, [sceneIntentLinks, autoSceneRules]);
  const activeIntent = resolveSceneIntent(activeScene);
  const activeSceneRule =
    autoSceneRules.find((rule) => sceneIntentLinks[rule.id] === scenes.activeSceneId) ||
    autoSceneRules.find((rule) => rule.isDefault) ||
    autoSceneRules[0] ||
    null;
  const activeRuleLinkedScene =
    activeSceneRule
      ? allScenes.find((scene) => scene.id === (sceneIntentLinks[activeSceneRule.id] || "")) || null
      : null;
  const activeRuleSummary = activeSceneRule
    ? `${activeSceneRule.thresholdEnabled
        ? `<= ${activeSceneRule.thresholdMbps == null ? "unset" : `${Number(activeSceneRule.thresholdMbps).toFixed(1)} Mbps`}`
        : "Threshold off"} -> ${activeRuleLinkedScene?.name || "Unlinked"}`
    : "No auto rule mapped";
  const activeSceneDisplayName = activeSceneRule?.label || activeScene?.name || "No active scene";
  const activeRuleIntentColor = SCENE_INTENT_COLORS[activeSceneRule?.intent] || SCENE_INTENT_COLORS.OFFLINE;
  const activeRuleColor = normalizeOptionalHexColor(activeSceneRule?.bgColor) || activeRuleIntentColor.bg;
  const activeRuleBorderColor = normalizeOptionalHexColor(activeSceneRule?.bgColor) || activeRuleIntentColor.border;
  const autoSceneSwitchEnabled =
    typeof scenes.autoSwitchEnabled === "boolean"
      ? scenes.autoSwitchEnabled
      : (typeof settingsByKey.auto_scene_switch === "boolean" ? settingsByKey.auto_scene_switch : null);
  const manualOverrideEnabled =
    typeof scenes.manualOverrideEnabled === "boolean"
      ? scenes.manualOverrideEnabled
      : (typeof settingsByKey.manual_override === "boolean" ? settingsByKey.manual_override : null);
  const autoSwitchArmed =
    typeof scenes.autoSwitchArmed === "boolean"
      ? scenes.autoSwitchArmed
      : (typeof autoSceneSwitchEnabled === "boolean"
          ? (manualOverrideEnabled === true ? false : autoSceneSwitchEnabled)
          : false);
  const autoSwitchSource = typeof manualOverrideEnabled === "boolean" ? "manual_override" : "auto_scene_switch";

  const uiActionGateRef = useRef(new Map());
  const tryEnterUiActionGate = useCallback((gateKey, cooldownMs = 400) => {
    const now = Date.now();
    const until = uiActionGateRef.current.get(gateKey) || 0;
    if (until > now) return false;
    uiActionGateRef.current.set(gateKey, now + cooldownMs);
    return true;
  }, []);

  const [autoSwitchToggleLock, setAutoSwitchToggleLock] = useState(null);
  const autoSwitchToggleLockTimerRef = useRef(null);

  const clearAutoSwitchToggleLock = useCallback(() => {
    if (autoSwitchToggleLockTimerRef.current) {
      clearTimeout(autoSwitchToggleLockTimerRef.current);
      autoSwitchToggleLockTimerRef.current = null;
    }
    setAutoSwitchToggleLock(null);
  }, []);

  useEffect(() => {
    if (!autoSwitchToggleLock) return;
    if (autoSwitchArmed === autoSwitchToggleLock.targetArmed) {
      clearAutoSwitchToggleLock();
    }
  }, [autoSwitchArmed, autoSwitchToggleLock, clearAutoSwitchToggleLock]);

  useEffect(() => {
    const validSceneIds = new Set((scenes.items || []).map((s) => s.id));
    setSceneIntentLinks((prev) => {
      const next = { ...prev };
      let changed = false;
      autoSceneRules.forEach((rule) => {
        if (next[rule.id] && !validSceneIds.has(next[rule.id])) {
          next[rule.id] = "";
          changed = true;
        }
      });
      return changed ? next : prev;
    });
  }, [scenes.items, autoSceneRules]);

  useEffect(() => {
    if (!Array.isArray(scenes.items) || scenes.items.length === 0) return;
    const validSceneIds = new Set((scenes.items || []).map((s) => s.id));
    setSceneIntentLinks((prev) => {
      const next = { ...prev };
      let changed = false;
      autoSceneRules.forEach((rule) => {
        const currentId = String(next[rule.id] || "");
        if (currentId && validSceneIds.has(currentId)) return;
        const sceneName = String(sceneIntentLinkNames[rule.id] || "");
        if (!sceneName) return;
        const remappedId = findSceneIdByName(sceneName, scenes.items);
        if (!remappedId) return;
        if (next[rule.id] !== remappedId) {
          next[rule.id] = remappedId;
          changed = true;
        }
      });
      return changed ? next : prev;
    });
  }, [scenes.items, autoSceneRules, sceneIntentLinkNames]);

  useEffect(() => {
    if (!Array.isArray(scenes.items) || scenes.items.length === 0) return;
    setSceneIntentLinks((prev) => {
      const next = { ...prev };
      let changed = false;
      autoSceneRules.forEach((rule) => {
        if (next[rule.id]) return;
        const matchedSceneId = findBestSceneIdForRule(rule, scenes.items);
        if (matchedSceneId) {
          next[rule.id] = matchedSceneId;
          changed = true;
        }
      });
      return changed ? next : prev;
    });
  }, [scenes.items, autoSceneRules]);

  useEffect(() => {
    if (!Array.isArray(scenes.items) || scenes.items.length === 0) return;
    setSceneIntentLinkNames((prev) => {
      const next = { ...prev };
      let changed = false;
      autoSceneRules.forEach((rule) => {
        const sceneId = String(sceneIntentLinks[rule.id] || "");
        if (!sceneId) return;
        const scene = scenes.items.find((s) => String(s.id || "") === sceneId);
        if (!scene?.name) return;
        if (next[rule.id] !== scene.name) {
          next[rule.id] = scene.name;
          changed = true;
        }
      });
      return changed ? next : prev;
    });
  }, [scenes.items, autoSceneRules, sceneIntentLinks]);

  useEffect(() => {
    try {
      if (typeof window === "undefined") return;
      const storage = window.localStorage;
      if (!storage) return;
      storage.setItem(SCENE_LINK_STORAGE_KEY, JSON.stringify(sceneIntentLinks));
    } catch (_) {}
  }, [sceneIntentLinks]);

  useEffect(() => {
    try {
      if (typeof window === "undefined") return;
      const storage = window.localStorage;
      if (!storage) return;
      storage.setItem(SCENE_LINK_NAME_STORAGE_KEY, JSON.stringify(sceneIntentLinkNames));
    } catch (_) {}
  }, [sceneIntentLinkNames]);

  // autoSwitchSourceSelection localStorage save removed — no longer user-selectable

  useEffect(() => {
    try {
      if (typeof window === "undefined") return;
      const storage = window.localStorage;
      if (!storage) return;
      storage.setItem(AUTO_SCENE_RULES_STORAGE_KEY, JSON.stringify(autoSceneRules));
    } catch (_) {}
  }, [autoSceneRules]);

  useEffect(() => {
    if (!useBridge) {
      setScenePrefsHydrated(true);
      return;
    }

    setScenePrefsHydrated(false);
    const loadRequestId = `dockprefs_load_${Date.now()}`;
    try {
      sendAction({ type: "load_scene_prefs", requestId: loadRequestId });
    } catch (_) {}

    const hydrationTimeout = setTimeout(() => {
      setScenePrefsHydrated(true);
    }, 1500);

    const handler = (e) => {
      const result = e?.detail || {};
      if (result.actionType !== "load_scene_prefs") return;
      if (result.requestId !== loadRequestId) return;
      if (result.status !== "completed" || !result.ok) {
        setScenePrefsHydrated(true);
        return;
      }
      try {
        const raw = JSON.parse(result.detail || "{}");
        if (raw && typeof raw === "object") {
          if (raw.sceneIntentLinks && typeof raw.sceneIntentLinks === "object") {
            setSceneIntentLinks(normalizeLinkMap(raw.sceneIntentLinks));
          }
          if (raw.sceneIntentLinksByName && typeof raw.sceneIntentLinksByName === "object") {
            setSceneIntentLinkNames(normalizeLinkMap(raw.sceneIntentLinksByName));
          }
          // autoSwitchSourceSelection no longer loaded — derived from relayActive
          if (Array.isArray(raw.autoSceneRules)) {
            setAutoSceneRules(normalizeAutoSceneRulesValue(raw.autoSceneRules));
          }
        }
      } catch (_) {
        // keep defaults when persisted payload is invalid
      } finally {
        setScenePrefsHydrated(true);
      }
    };

    window.addEventListener("aegis:dock:action-native-result", handler);
    return () => {
      clearTimeout(hydrationTimeout);
      window.removeEventListener("aegis:dock:action-native-result", handler);
    };
  }, [useBridge, sendAction]);

  useEffect(() => {
    if (!useBridge) return;
    if (!scenePrefsHydrated) return;
    if (scenePrefsSaveTimerRef.current) {
      clearTimeout(scenePrefsSaveTimerRef.current);
    }
    scenePrefsSaveTimerRef.current = setTimeout(() => {
      const payload = {
        sceneIntentLinks,
        sceneIntentLinksByName: sceneIntentLinkNames,
        autoSceneRules,
      };
      try {
        sendAction({
          type: "save_scene_prefs",
          requestId: `dockprefs_save_${Date.now()}`,
          prefsJson: JSON.stringify(payload),
        });
      } catch (_) {}
    }, 300);
    return () => {
      if (scenePrefsSaveTimerRef.current) {
        clearTimeout(scenePrefsSaveTimerRef.current);
        scenePrefsSaveTimerRef.current = null;
      }
    };
  }, [useBridge, scenePrefsHydrated, sceneIntentLinks, sceneIntentLinkNames, autoSceneRules, sendAction]);

  useEffect(() => {
    return () => {
      if (autoSwitchToggleLockTimerRef.current) {
        clearTimeout(autoSwitchToggleLockTimerRef.current);
        autoSwitchToggleLockTimerRef.current = null;
      }
    };
  }, []);

  // Animated bitrate values
  const bondedKbps = bitrate.bondedKbps || 0;
  const relayBondedKbps = bitrate.relayBondedKbps || bondedKbps;
  // Encoders & Uploads — per-output grouped data
  const encoderOutputs = ds.outputs || { groups: [], hidden: [] };
  const allEncoderItems = encoderOutputs.groups?.flatMap(g => g.items) || [];
  const activeOutputCount = allEncoderItems.filter(o => o.active).length;
  const { maxMap: outputMaxMap, sectionMax: outputSectionMax } = useRollingMaxBitrate(allEncoderItems);
  const link1Bitrate = conns[0]?.bitrate || 0;
  const link2Bitrate = conns[1]?.bitrate || 0;
  const animLink1  = useAnimatedValue(link1Bitrate, 600);
  const animLink2  = useAnimatedValue(link2Bitrate, 600);
  const animBonded = useAnimatedValue(bondedKbps, 600);
  const animRelayBonded = useAnimatedValue(relayBondedKbps, 600);
  const maxPerLink = bitrate.maxPerLinkKbps || 6000;
  const maxBonded  = bitrate.maxBondedKbps || 12000;
  // Auto-switch bitrate derived from relay state — only used when relayActive
  const autoSwitchBitrateKbps = relayActive ? relayBondedKbps : bondedKbps;

  // --- Dispatch helpers ---
  const handleSceneSwitch = (scene) => {
    if (!scene?.id) return;
    if (!tryEnterUiActionGate(`switch_scene:${scene.id}`, UI_ACTION_COOLDOWNS_MS.switchScene)) return;
    if (autoSwitchArmed && tryEnterUiActionGate("manual_scene_lockout", UI_ACTION_COOLDOWNS_MS.setSetting)) {
      const requestId = genRequestId();
      if (autoSwitchSource === "manual_override") {
        sendAction({
          type: "set_setting",
          key: "manual_override",
          value: true,
          requestId,
          reason: "manual_scene_switch",
        });
      } else {
        sendAction({
          type: "set_setting",
          key: "auto_scene_switch",
          value: false,
          requestId,
          reason: "manual_scene_switch",
        });
      }
    }
    sendAction({ type: "switch_scene", sceneId: scene.id, sceneName: scene.name });
  };

  // handleModeChange removed — mode is now derived from relay state
  const handleRelayToggle = () => {
    if (relayActivating) return;
    if (relayActive) {
      sendAction({ type: "relay_stop" });
    } else {
      setRelayActivating(true);
      setRelayError(null);
      sendAction({ type: "relay_start" });
      // Timeout fallback — clear activating state if no result after 15s
      setTimeout(() => setRelayActivating((prev) => {
        if (prev) setRelayError("Activation timed out");
        return false;
      }), 15000);
    }
  };

  const handleSettingChange = (key, newValue) => {
    if (!tryEnterUiActionGate(`set_setting:${key}`, UI_ACTION_COOLDOWNS_MS.setSetting)) return;
    sendAction({ type: "set_setting", key, value: newValue });
  };

  const handleAutoSceneSwitchToggle = () => {
    if (autoSwitchToggleLock) return;
    if (!tryEnterUiActionGate("set_setting:auto_scene_switch", UI_ACTION_COOLDOWNS_MS.autoSceneSwitch)) return;
    const targetArmed = !autoSwitchArmed;
    const requestId = genRequestId();
    setAutoSwitchToggleLock({ requestId, targetArmed });
    autoSwitchToggleLockTimerRef.current = setTimeout(() => {
      autoSwitchToggleLockTimerRef.current = null;
      setAutoSwitchToggleLock(null);
    }, AUTO_SWITCH_LOCK_TIMEOUT_MS);
    if (autoSwitchSource === "manual_override") {
      sendAction({
        type: "set_setting",
        key: "manual_override",
        value: !targetArmed,
        requestId,
      });
      return;
    }
    sendAction({
      type: "set_setting",
      key: "auto_scene_switch",
      value: targetArmed,
      requestId,
    });
  };

  const setSceneIntentLink = useCallback((ruleId, sceneId) => {
    const nextSceneId = String(sceneId || "");
    const scene = allScenes.find((s) => String(s.id || "") === nextSceneId) || null;
    setSceneIntentLinks((prev) => ({ ...prev, [ruleId]: nextSceneId }));
    setSceneIntentLinkNames((prev) => ({ ...prev, [ruleId]: String(scene?.name || "") }));
  }, [allScenes]);

  const updateAutoSceneRule = useCallback((ruleId, patch) => {
    setAutoSceneRules((prev) =>
      prev.map((rule) => (rule.id === ruleId ? { ...rule, ...patch } : rule)));
  }, []);

  const addAutoSceneRule = useCallback(() => {
    const id = `rule_${Date.now()}_${Math.floor(Math.random() * 1000)}`;
    setAutoSceneRules((prev) => ([
      ...prev,
      { id, label: `Custom ${prev.length + 1}`, intent: "HOLD", thresholdEnabled: true, thresholdMbps: 0.5, isDefault: false, bgColor: "#3a2a1a" },
    ]));
    setExpandedRuleId(id);
  }, []);

  const removeAutoSceneRule = useCallback((ruleId) => {
    setAutoSceneRules((prev) => (prev.length <= 1 ? prev : prev.filter((rule) => rule.id !== ruleId)));
    setExpandedRuleId((prev) => (prev === ruleId ? null : prev));
    setSceneIntentLinks((prev) => {
      const next = { ...prev };
      delete next[ruleId];
      return next;
    });
    setSceneIntentLinkNames((prev) => {
      const next = { ...prev };
      delete next[ruleId];
      return next;
    });
  }, []);

  useEffect(() => {
    if (!relayActive || !autoSwitchArmed) return;
    if ((scenes.items || []).length === 0) return;
    if (scenes.pendingSceneId) return;
    const mbps = autoSwitchBitrateKbps / 1000;
    const thresholdRules = autoSceneRules
      .filter((rule) => rule.thresholdEnabled && rule.thresholdMbps != null && Number.isFinite(rule.thresholdMbps))
      .sort((a, b) => (a.thresholdMbps - b.thresholdMbps));
    let targetRule = null;
    for (const rule of thresholdRules) {
      if (mbps <= rule.thresholdMbps) {
        targetRule = rule;
        break;
      }
    }
    if (!targetRule) {
      targetRule = autoSceneRules.find((rule) => rule.isDefault) || autoSceneRules[0] || null;
    }
    if (!targetRule) return;
    const targetSceneId = sceneIntentLinks[targetRule.id];
    if (!targetSceneId || targetSceneId === scenes.activeSceneId) return;
    const targetScene = (scenes.items || []).find((s) => s.id === targetSceneId);
    if (!targetScene) return;
    if (!tryEnterUiActionGate(`auto_profile_switch:${targetSceneId}`, 2500)) return;
    sendAction({
      type: "switch_scene",
      sceneId: targetScene.id,
      sceneName: targetScene.name,
      reason: `auto_profile_${targetRule.id}`,
    });
  }, [
    relayActive,
    autoSwitchArmed,
    scenes.items,
    scenes.pendingSceneId,
    scenes.activeSceneId,
    autoSwitchBitrateKbps,
    autoSceneRules,
    sceneIntentLinks,
    sendAction,
    tryEnterUiActionGate,
  ]);

  // Version display
  const version = header.version || "v0.0.3";
  const relayStatusUi = mapRelayStatusForUi(relay.status);

  // Merge runtime theme with defaults for safer property access
  const activeTheme = { ...OBS_YAMI_GREY_DEFAULTS, ...theme };
  const themeFontFamily = (typeof activeTheme.fontFamily === "string" && activeTheme.fontFamily.trim())
    ? `'${activeTheme.fontFamily.replace(/'/g, "\\'")}', 'JetBrains Mono', 'SF Mono', 'Cascadia Code', monospace`
    : "'JetBrains Mono', 'SF Mono', 'Cascadia Code', monospace";
  const isLightTheme = useMemo(() => (
    isLightColor(activeTheme.bg) || isLightColor(activeTheme.surface)
  ), [activeTheme.bg, activeTheme.surface]);

  return (
    <div ref={dockRootRef} style={{
      width: "100%", height: "100%",
      background: activeTheme.bg,
      display: "flex", flexDirection: "column",
      fontFamily: themeFontFamily,
      color: activeTheme.text,
      position: "relative",
      overflow: "hidden",
      "--theme-bg": activeTheme.bg,
      "--theme-surface": activeTheme.surface,
      "--theme-panel": activeTheme.panel,
      "--theme-text": activeTheme.text,
      "--theme-text-muted": activeTheme.textMuted,
      "--theme-accent": activeTheme.accent,
      "--theme-border": activeTheme.border,
      "--theme-scrollbar": activeTheme.scrollbar,
      "--theme-font-family": themeFontFamily,
    }}>
      <style>{getDockCss(activeTheme)}</style>

      {/* ================================================================= */}
      {/* HEALTH ACCENT RAIL — top edge status indicator                     */}
      {/* ================================================================= */}
      <div style={{
        height: 2, flexShrink: 0,
        background: `linear-gradient(90deg, transparent 0%, ${healthColor} 40%, ${healthColor} 60%, transparent 100%)`,
        boxShadow: `0 0 12px ${healthColor}30, 0 1px 4px ${healthColor}20`,
        animation: "railPulse 3s ease-in-out infinite",
        transition: "background 0.6s ease, box-shadow 0.6s ease",
      }} />

      {/* ================================================================= */}
      {/* HEADER                                                            */}
      {/* ================================================================= */}
      <div style={{
        padding: isCompact ? "8px 10px 7px" : "10px 12px 8px", flexShrink: 0,
        background: `linear-gradient(180deg, var(--theme-panel, #12141a) 0%, var(--theme-bg, #0e1015) 100%)`,
        borderBottom: "1px solid var(--theme-border, #1a1d23)",
        display: "flex", alignItems: "center", gap: isCompact ? 6 : 8,
        flexWrap: isCompact ? "wrap" : "nowrap",
      }}>
        {/* Logo */}
        <div style={{
          width: isCompact ? 22 : 26, height: isCompact ? 22 : 26, borderRadius: 5,
          background: `linear-gradient(135deg, ${healthColor}dd 0%, ${healthColor}88 100%)`,
          display: "flex", alignItems: "center", justifyContent: "center",
          boxShadow: `0 2px 8px ${healthColor}25`,
          transition: "background 0.6s ease, box-shadow 0.6s ease",
        }}>
          <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
            <path d="M7 1L12.5 4.25V10.75L7 14L1.5 10.75V4.25L7 1Z" stroke="currentColor" strokeWidth="1.5" fill="none"/>
            <circle cx="7" cy="7.5" r="2" fill="currentColor" opacity="0.9"/>
          </svg>
        </div>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{
            fontSize: isCompact ? 11 : 12, fontWeight: 700, color: "var(--theme-text, #e8eaef)", letterSpacing: "0.04em",
            fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
          }}>
            {header.title || "Telemy Aegis"}
          </div>
          <div style={{
            fontSize: isCompact ? 7 : 8, color: "var(--theme-text-muted, #5a5f6d)", fontWeight: 500, letterSpacing: "0.06em",
            textTransform: "uppercase",
            overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
          }}>
            {header.subtitle || "OBS + CORE IPC DOCK"}
          </div>
        </div>
        {/* Mode Badge — derived from relay state (informational only) */}
        <div style={{
          display: "flex", alignItems: "center", gap: 4,
          background: derivedMode === "irl" ? "var(--theme-accent, #1a3a5a)" : "var(--theme-surface, #13151a)",
          borderRadius: 4, padding: isCompact ? "4px 8px" : "5px 10px",
          border: `1px solid ${derivedMode === "irl" ? "#2d7aed40" : "var(--theme-border, #1e2028)"}`,
          flexShrink: 0,
        }}>
          <div style={{
            width: 5, height: 5, borderRadius: "50%",
            background: derivedMode === "irl" ? "#2d7aed" : "var(--theme-text-muted, #5a5f6d)",
          }} />
          <span style={{
            fontSize: isCompact ? 8 : 9, fontWeight: 600,
            fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
            textTransform: "uppercase", letterSpacing: "0.06em",
            color: derivedMode === "irl" ? "#5ba3f5" : "var(--theme-text-muted, #8b8f98)",
          }}>
            {derivedMode}
          </span>
        </div>
      </div>

      {/* ================================================================= */}
      {/* LIVE STATUS BANNER                                                */}
      {/* ================================================================= */}
      <div style={{
        padding: isCompact ? "6px 10px" : "7px 12px", flexShrink: 0,
        background: isLive
          ? `linear-gradient(90deg, ${HEALTH_COLORS.healthy}18 0%, var(--theme-bg, #0e1015) 100%)`
          : `linear-gradient(90deg, var(--theme-surface, #1a1a2a) 0%, var(--theme-bg, #0e1015) 100%)`,
        borderBottom: "1px solid var(--theme-border, #1a1d23)",
        display: "flex", alignItems: "center", gap: isCompact ? 6 : 8,
        flexWrap: isUltraCompact ? "wrap" : "nowrap",
      }}>
        <div style={{
          width: 8, height: 8, borderRadius: "50%",
          background: isLive ? "#2ea043" : "#4a4f5c",
          boxShadow: isLive ? "0 0 8px #2ea04360" : "none",
          animation: isLive ? "pulse 2s ease-in-out infinite" : "none",
          flexShrink: 0,
        }} />
        <span style={{
          fontSize: isCompact ? 9 : 10, fontWeight: 700,
          color: isLive ? "#4ade80" : "var(--theme-text-muted, #5a5f6d)",
          letterSpacing: "0.08em", textTransform: "uppercase",
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
        }}>
          {isLive ? "LIVE" : derivedMode.toUpperCase()}
        </span>
        <span style={{ fontSize: isCompact ? 9 : 10, color: "var(--theme-text-muted, #5a5f6d)", marginLeft: "auto" }}>
          {formatTime(elapsedSec)}
        </span>
        <span style={{
          fontSize: isCompact ? 8 : 9, color: "var(--theme-text-muted, #8b8f98)", background: "var(--theme-surface, #1a1d23)",
          padding: isCompact ? "1px 5px" : "2px 6px", borderRadius: 2,
        }}>
          {(animBonded / 1000).toFixed(1)} Mbps
        </span>
      </div>

      {/* ================================================================= */}
      {/* SCROLLABLE SECTIONS — flex:1 fills remaining height               */}
      {/* ================================================================= */}
      <div className="aegis-dock-scroll" style={{
        flex: 1, minHeight: 0, overflowY: "auto", overflowX: "hidden",
      }}>

        {/* ----- SCENES ----- */}
        <Section title="Scenes" icon="◉" defaultOpen={true} compact={isCompact}
          badge={activeIntent}
          badgeColor={SCENE_INTENT_COLORS[activeIntent]?.border || "#4a4f5c"}>
          <div style={{
            marginBottom: 6,
            padding: "6px 7px",
            borderRadius: 4,
            border: `1px solid ${toRgba(activeRuleBorderColor, 0.55)}`,
            background: isLightTheme
              ? `linear-gradient(135deg, ${toRgba(activeRuleColor, 0.22)} 0%, var(--theme-surface, #13151a) 100%)`
              : `linear-gradient(135deg, ${toRgba(activeRuleColor, 0.65)} 0%, var(--theme-surface, #13151a) 100%)`,
          }}>
            <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
              <span style={{
                width: 6,
                height: 6,
                borderRadius: "50%",
                background: activeScene ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)",
                boxShadow: activeScene ? "0 0 6px var(--theme-accent, #5ba3f5)" : "none",
                flexShrink: 0,
              }} />
              <span style={{
                fontSize: isCompact ? 9 : 10,
                color: "var(--theme-text-muted, #8b8f98)",
                letterSpacing: "0.04em",
                textTransform: "uppercase",
                fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                flexShrink: 0,
              }}>
                Active
              </span>
              <span style={{ minWidth: 0, flex: 1, display: "flex", flexDirection: "column", lineHeight: 1.15 }}>
                <span style={{
                  fontSize: isCompact ? 10 : 11,
                  color: "var(--theme-text, #e0e2e8)",
                  fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                  overflow: "hidden",
                  textOverflow: "ellipsis",
                  whiteSpace: "nowrap",
                }}>
                  {activeSceneDisplayName}
                </span>
                <span style={{
                  fontSize: 8,
                  color: "var(--theme-text-muted, #8b8f98)",
                  fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                  overflow: "hidden",
                  textOverflow: "ellipsis",
                  whiteSpace: "nowrap",
                }}>
                  {activeRuleSummary}
                </span>
              </span>
              {pendingScene && (
                <span style={{
                  fontSize: 8,
                  color: "var(--theme-accent, #5ba3f5)",
                  fontWeight: 700,
                  letterSpacing: "0.05em",
                }}>
                  SWITCHING
                </span>
              )}
              <button
                type="button"
                onClick={() => setScenePanelExpanded((prev) => !prev)}
                style={{
                  width: 20,
                  height: 20,
                  borderRadius: 3,
                  border: "1px solid var(--theme-border, #2a2d35)",
                  background: "var(--theme-panel, #20232b)",
                  color: scenePanelExpanded ? "var(--theme-accent, #5ba3f5)" : "var(--theme-text-muted, #8b8f98)",
                  fontSize: 11,
                  lineHeight: 1,
                  padding: 0,
                  cursor: "pointer",
                  flexShrink: 0,
                }}
                title={scenePanelExpanded ? "Collapse advanced scene controls" : "Expand advanced scene controls"}>
                {scenePanelExpanded ? "▴" : "▾"}
              </button>
            </div>
          </div>

          {scenePanelExpanded && (
            <>
          <div style={{
            display: "flex",
            alignItems: "center",
            gap: 6,
            marginBottom: 6,
            padding: "4px 6px",
            borderRadius: 4,
            border: "1px solid var(--theme-border, #2a2d35)",
            background: "var(--theme-surface, #13151a)",
          }}>
            <span style={{
              fontSize: isCompact ? 8 : 9,
              color: "var(--theme-text-muted, #8b8f98)",
              letterSpacing: "0.04em",
              textTransform: "uppercase",
              fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
              flexShrink: 0,
              flex: 1,
            }}>
              Scene Rules
            </span>
            <button
              type="button"
              onClick={addAutoSceneRule}
              style={{
                height: 21,
                borderRadius: 3,
                border: "1px solid var(--theme-border, #2a2d35)",
                background: "var(--theme-panel, #20232b)",
                color: "var(--theme-text, #e0e2e8)",
                fontSize: 10,
                padding: "0 7px",
                cursor: "pointer",
                fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
              }}>
              + Rule
            </button>
          </div>
          {autoSceneRules.map((row) => {
            const linkedSceneId = sceneIntentLinks[row.id] || "";
            const linkedScene = (allScenes || []).find((scene) => scene.id === linkedSceneId) || null;
            const isActive = linkedScene && linkedScene.id === scenes.activeSceneId;
            const isPending = linkedScene && linkedScene.id === scenes.pendingSceneId;
            const intentColor = SCENE_INTENT_COLORS[row.intent] || SCENE_INTENT_COLORS.OFFLINE;
            const isExpanded = expandedRuleId === row.id;
            const ruleBgColor = normalizeOptionalHexColor(row.bgColor) || intentColor.bg;
            const ruleBorderColor = normalizeOptionalHexColor(row.bgColor) || intentColor.border;
            const activeRowBg = isLightTheme ? toRgba(ruleBgColor, 0.14) : ruleBgColor;
            const activeRowText = (isLightTheme || isLightColor(ruleBgColor))
              ? "var(--theme-text, #1b1d22)"
              : "var(--theme-text, #e0e2e8)";
            return (
              <div key={row.id} style={{
                display: "flex",
                flexDirection: "column",
                gap: 5,
                marginBottom: 5,
                padding: "5px 6px",
                borderRadius: 4,
                border: `1px solid ${isActive ? ruleBorderColor : "var(--theme-border, #2a2d35)"}`,
                background: isActive ? activeRowBg : "var(--theme-surface, #13151a)",
              }}>
                <div style={{ display: "grid", gridTemplateColumns: "minmax(0, 1fr) auto", gap: 4, alignItems: "center" }}>
                  <button
                    type="button"
                    onClick={() => linkedScene && handleSceneSwitch(linkedScene)}
                    disabled={!linkedScene}
                    style={{
                      display: "flex",
                      alignItems: "flex-start",
                      gap: 6,
                      border: "none",
                      background: "transparent",
                      color: isActive ? activeRowText : "var(--theme-text, #e0e2e8)",
                      cursor: linkedScene ? "pointer" : "default",
                      textAlign: "left",
                      padding: 0,
                      minWidth: 0,
                    }}>
                    <span style={{
                      width: 5,
                      height: 5,
                      borderRadius: "50%",
                      background: isActive ? ruleBorderColor : (isPending ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)"),
                      boxShadow: isActive ? `0 0 4px ${ruleBorderColor}` : "none",
                      flexShrink: 0,
                    }} />
                    <span style={{
                      display: "flex",
                      flexDirection: "column",
                      minWidth: 0,
                      lineHeight: 1.15,
                    }}>
                      <span style={{
                        fontSize: isCompact ? 9 : 10,
                        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                        overflow: "hidden",
                        textOverflow: "ellipsis",
                        whiteSpace: "nowrap",
                        color: isActive ? activeRowText : "var(--theme-text, #e0e2e8)",
                      }}>
                        {row.label}
                      </span>
                      <span style={{
                        fontSize: 8,
                        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                        overflow: "hidden",
                        textOverflow: "ellipsis",
                        whiteSpace: "nowrap",
                        color: "var(--theme-text-muted, #8b8f98)",
                      }}>
                        {`${row.thresholdEnabled ? `<= ${row.thresholdMbps == null ? "unset" : `${Number(row.thresholdMbps).toFixed(1)} Mbps`}` : "Threshold off"} -> ${linkedScene?.name || "Unlinked"}`}
                      </span>
                    </span>
                  </button>
                  <div style={{ display: "flex", alignItems: "center", gap: 3 }}>
                    {isExpanded && (
                      <>
                        {RULE_BG_PRESETS.map((preset) => {
                          const selected = normalizeOptionalHexColor(row.bgColor) === normalizeOptionalHexColor(preset.color);
                          return (
                            <button
                              key={`${row.id}_${preset.id}`}
                              type="button"
                              onClick={() => updateAutoSceneRule(row.id, { bgColor: preset.color })}
                              title={preset.label}
                              style={{
                                width: 14,
                                height: 14,
                                borderRadius: 3,
                                border: selected ? "1px solid var(--theme-accent, #5ba3f5)" : "1px solid var(--theme-border, #2a2d35)",
                                background: preset.color,
                                boxShadow: selected ? `0 0 0 1px ${toRgba(preset.color, 0.55)}` : "none",
                                padding: 0,
                                cursor: "pointer",
                              }}
                            />
                          );
                        })}
                        <label
                          title="Custom color"
                          style={{
                            width: 15,
                            height: 15,
                            borderRadius: "50%",
                            border: "1px solid var(--theme-border, #2a2d35)",
                            overflow: "hidden",
                            cursor: "pointer",
                            display: "inline-block",
                            position: "relative",
                            background: normalizeOptionalHexColor(row.bgColor) || "#2a2d35",
                          }}>
                          <input
                            type="color"
                            value={normalizeOptionalHexColor(row.bgColor) || "#2a2d35"}
                            onChange={(e) => updateAutoSceneRule(row.id, { bgColor: e.target.value })}
                            style={{
                              position: "absolute",
                              inset: 0,
                              width: "100%",
                              height: "100%",
                              border: "none",
                              padding: 0,
                              margin: 0,
                              background: "transparent",
                              cursor: "pointer",
                              opacity: 0,
                            }}
                          />
                        </label>
                      </>
                    )}
                    <button
                      type="button"
                      onClick={() => setExpandedRuleId((prev) => (prev === row.id ? null : row.id))}
                      style={{
                        height: 21,
                        borderRadius: 3,
                        border: "1px solid var(--theme-border, #2a2d35)",
                        background: "var(--theme-panel, #20232b)",
                        color: isExpanded ? "var(--theme-accent, #5ba3f5)" : "var(--theme-text-muted, #8b8f98)",
                        fontSize: 9,
                        padding: "0 8px",
                        cursor: "pointer",
                      }}>
                      {isExpanded ? "Close" : "Edit"}
                    </button>
                  </div>
                </div>
                {isExpanded && (
                  <div style={{
                    display: "grid",
                    gridTemplateColumns: "minmax(0, 1fr) auto auto minmax(0, 1fr) auto",
                    gap: 4,
                    alignItems: "center",
                    paddingTop: 2,
                  }}>
                    <input
                      value={row.label}
                      onChange={(e) => updateAutoSceneRule(row.id, { label: e.target.value.slice(0, 40) })}
                      style={{
                        height: 21,
                        borderRadius: 3,
                        border: "1px solid var(--theme-border, #2a2d35)",
                        background: "var(--theme-panel, #20232b)",
                        color: "var(--theme-text, #e0e2e8)",
                        fontSize: 9,
                        padding: "0 6px",
                        minWidth: 80,
                        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                      }}
                    />
                    <input
                      type="number"
                      step="0.1"
                      min="0"
                      value={row.thresholdMbps == null ? "" : row.thresholdMbps}
                      onChange={(e) => {
                        const v = e.target.value;
                        updateAutoSceneRule(row.id, {
                          thresholdEnabled: true,
                          thresholdMbps: v === "" ? null : Math.max(0, Number(v) || 0),
                        });
                      }}
                      title="Switch when bitrate is at or below this Mbps value"
                      disabled={!row.thresholdEnabled}
                      style={{
                        width: 58,
                        height: 21,
                        borderRadius: 3,
                        border: "1px solid var(--theme-border, #2a2d35)",
                        background: "var(--theme-panel, #20232b)",
                        color: "var(--theme-text, #e0e2e8)",
                        fontSize: 9,
                        padding: "0 4px",
                        opacity: row.thresholdEnabled ? 1 : 0.55,
                        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                      }}
                      placeholder="-"
                    />
                    <label style={{
                      display: "flex",
                      alignItems: "center",
                      gap: 4,
                      fontSize: 8,
                      color: "var(--theme-text-muted, #8b8f98)",
                      fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                      whiteSpace: "nowrap",
                    }}>
                      <input
                        type="checkbox"
                        checked={!!row.thresholdEnabled}
                        onChange={(e) => {
                          const enabled = e.target.checked;
                          updateAutoSceneRule(row.id, {
                            thresholdEnabled: enabled,
                            thresholdMbps: enabled ? (row.thresholdMbps == null ? 0.5 : row.thresholdMbps) : row.thresholdMbps,
                          });
                        }}
                      />
                      Threshold
                    </label>
                    <select
                      value={linkedSceneId}
                      onChange={(e) => setSceneIntentLink(row.id, e.target.value)}
                      style={{
                        height: 21,
                        borderRadius: 3,
                        border: "1px solid var(--theme-border, #2a2d35)",
                        background: "var(--theme-panel, #20232b)",
                        color: "var(--theme-text, #e0e2e8)",
                        fontSize: 9,
                        padding: "0 4px",
                        minWidth: 110,
                        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                      }}>
                      <option value="">Select scene...</option>
                      {(allScenes || []).map((scene) => (
                        <option key={scene.id} value={scene.id}>{scene.name}</option>
                      ))}
                    </select>
                    <button
                      type="button"
                      onClick={() => removeAutoSceneRule(row.id)}
                      disabled={autoSceneRules.length <= 1}
                      style={{
                        height: 21,
                        borderRadius: 3,
                        border: "1px solid var(--theme-border, #2a2d35)",
                        background: "var(--theme-panel, #20232b)",
                        color: "var(--theme-text-muted, #8b8f98)",
                        fontSize: 10,
                        padding: "0 6px",
                        cursor: autoSceneRules.length <= 1 ? "not-allowed" : "pointer",
                        opacity: autoSceneRules.length <= 1 ? 0.5 : 1,
                      }}>
                      Remove
                    </button>
                  </div>
                )}
              </div>
            );
          })}
            </>
          )}

          {/* Auto Scene Switch quick toggle */}
          <button
            type="button"
            onClick={handleAutoSceneSwitchToggle}
            disabled={!!autoSwitchToggleLock}
            style={{
            marginTop: 8, padding: isCompact ? "5px 7px" : "6px 8px", background: "var(--theme-surface, #1a1d23)",
            borderRadius: 4, display: "flex", alignItems: "center", gap: 6,
            border: "1px solid var(--theme-border, #2a2d35)",
            width: "100%",
            cursor: autoSwitchToggleLock ? "not-allowed" : "pointer",
            textAlign: "left",
            opacity: autoSwitchToggleLock ? 0.75 : 1,
          }}>
            <svg width="10" height="10" viewBox="0 0 10 10" fill="none">
              <path d="M1 5C1 2.8 2.8 1 5 1s4 1.8 4 4-1.8 4-4 4"
                stroke={autoSwitchArmed ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)"} strokeWidth="1.2" strokeLinecap="round"/>
              <path d="M3 7L1 5L3 3"
                stroke={autoSwitchArmed ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)"} strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round"/>
            </svg>
            <span style={{ fontSize: 9, color: autoSwitchArmed ? "var(--theme-accent, #5ba3f5)" : "var(--theme-text-muted, #5a5f6d)", fontWeight: 500 }}>
              Auto Scene Switch
            </span>
            <span style={{
              fontSize: isCompact ? 8 : 9, fontWeight: 600, marginLeft: "auto",
              color: autoSwitchArmed ? "#2ea043" : "#da3633",
            }}>
              {autoSwitchToggleLock ? (isCompact ? "..." : "APPLYING...") : (autoSwitchArmed ? "ARMED" : "MANUAL")}
            </span>
          </button>
        </Section>

        {/* Network Health section removed — Core Pipe now shown inside Relay section */}

        {/* ----- CONNECTIONS (visible when relay active) ----- */}
        {relayActive && conns.length > 0 && (
          <Section title="Connections" icon="⬡" defaultOpen={true} compact={isCompact}
            badge={String(conns.filter(c => c.status === "connected").length)}
            badgeColor="#2ea043">
            {conns.map((conn, i) => (
              <ConnectionCard
                key={conn.name || i}
                name={conn.name}
                type={conn.type}
                signal={conn.signal}
                bitrate={conn.bitrate || 0}
                status={conn.status}
                compact={isCompact}
              />
            ))}
          </Section>
        )}

        {/* ----- BITRATE ----- */}
        <Section title="Bitrate" icon="▥" defaultOpen={true} compact={isCompact}>
          {/* Per-link bars only when relay active (cellular links available) */}
          {relayActive && conns.length >= 2 ? (
            <>
              <BitrateBar value={animLink1} max={maxPerLink} color="#2d7aed"
                label={conns[0]?.name?.split(" \u2014 ")[0] || "LINK 1"} />
              <BitrateBar value={animLink2} max={maxPerLink} color="#8b5cf6"
                label={conns[1]?.name?.split(" \u2014 ")[0] || "LINK 2"} />
              <BitrateBar value={animBonded} max={maxBonded} color="#2ea043" label="BONDED" />
              <BitrateBar value={animRelayBonded} max={maxBonded} color="#5ba3f5" label="AWS RELAY INGEST" />
            </>
          ) : null}

          {/* Threshold indicators */}
          <div style={{
            marginTop: 8, display: "grid", gridTemplateColumns: isUltraCompact ? "1fr" : "1fr 1fr",
            gap: 4, fontSize: 9, fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
          }}>
            <div style={{
              background: "var(--theme-surface, #13151a)", borderRadius: 3, padding: "5px 7px",
              border: "1px solid var(--theme-border, #1e2028)",
            }}>
              <div style={{ color: "var(--theme-text-muted, #5a5f6d)", marginBottom: 2 }}>LOW THRESHOLD</div>
              <div style={{ color: "#fbbf24", fontWeight: 600 }}>
                {bitrate.lowThresholdMbps != null ? `${bitrate.lowThresholdMbps.toFixed(1)} Mbps` : "\u2014"}
              </div>
            </div>
            <div style={{
              background: "var(--theme-surface, #13151a)", borderRadius: 3, padding: "5px 7px",
              border: "1px solid var(--theme-border, #1e2028)",
            }}>
              <div style={{ color: "var(--theme-text-muted, #5a5f6d)", marginBottom: 2 }}>BRB THRESHOLD</div>
              <div style={{ color: "#da3633", fontWeight: 600 }}>
                {bitrate.brbThresholdMbps != null ? `${bitrate.brbThresholdMbps.toFixed(1)} Mbps` : "\u2014"}
              </div>
            </div>
          </div>

        </Section>

        {/* ----- ENCODERS & UPLOADS (always visible, per-output health) ----- */}
        {allEncoderItems.length > 0 && (
          <Section title="Encoders & Uploads" icon="&#x229e;" defaultOpen={true} compact={isCompact}
            badge={String(activeOutputCount)}
            badgeColor={activeOutputCount > 0 ? "#2ea043" : "var(--theme-border, #3a3d45)"}>
            {encoderOutputs.groups.map((group, gi) => (
              <div key={group.name || gi}>
                <EncoderGroupHeader
                  name={group.name}
                  resolution={group.resolution}
                  totalBitrateKbps={group.totalBitrateKbps}
                  avgLagMs={group.avgLagMs}
                  compact={isCompact}
                />
                {group.items.map((item, ii) => (
                  <OutputBar
                    key={item.id || `${gi}-${ii}`}
                    name={item.name || item.platform}
                    bitrateKbps={item.kbps}
                    fps={item.fps}
                    dropPct={item.dropPct}
                    active={item.active !== false}
                    maxBitrate={outputMaxMap[item.id || item.name || item.platform] || outputSectionMax}
                    compact={isCompact}
                  />
                ))}
              </div>
            ))}
            <HiddenOutputsToggle items={encoderOutputs.hidden} compact={isCompact} />
          </Section>
        )}

        {/* ----- RELAY (always visible, state-machine driven) ----- */}
        <Section title="Relay" icon="☁"
          compact={isCompact}
          defaultOpen={true}
          badge={!relayLicensed ? "PRO" : relayActive ? relayStatusUi.toUpperCase() : "OFF"}
          badgeColor={!relayLicensed ? "#d29922" : relayActive ? (relayStatusUi === "active" ? "#2d7aed" : relayStatusUi === "grace" ? "#8b5cf6" : relayStatusUi === "connecting" ? "#d29922" : "var(--theme-border, #3a3d45)") : "var(--theme-border, #3a3d45)"}>

          {/* Core Pipe — always visible */}
          <div style={{
            display: "flex", justifyContent: "space-between", alignItems: "center",
            background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "6px 10px",
            border: "1px solid var(--theme-border, #2a2d35)", marginBottom: 6, fontSize: 10,
          }}>
            <span style={{ color: "var(--theme-text-muted, #8b8f98)" }}>Core Pipe</span>
            <span style={{ color: pipeColor, fontWeight: 700 }}>{pipeLabel}</span>
          </div>

          {/* --- UNLICENSED state --- */}
          {!relayLicensed && (
            <div style={{
              background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "10px 10px",
              border: "1px solid var(--theme-border, #2a2d35)", textAlign: "center",
            }}>
              <div style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)", marginBottom: 6 }}>
                Relay is a Pro feature
              </div>
              <a href="https://telemyapp.com" target="_blank" rel="noopener noreferrer" style={{
                fontSize: 9, color: "#5ba3f5", textDecoration: "none", fontWeight: 600,
              }}>
                Upgrade at telemyapp.com &rarr;
              </a>
            </div>
          )}

          {/* --- IDLE state (licensed, not active) --- */}
          {relayLicensed && !relayActive && !relayActivating && (
            <div style={{
              background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "8px 10px",
              border: "1px solid var(--theme-border, #2a2d35)",
            }}>
              <button onClick={handleRelayToggle} style={{
                width: "100%", padding: "7px 0", border: "1px solid var(--theme-border, #2a2d35)",
                borderRadius: 4, background: "var(--theme-panel, #20232b)", cursor: "pointer",
                color: "#5ba3f5", fontSize: 10, fontWeight: 600,
                fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                letterSpacing: "0.04em",
              }}>
                Activate Relay
              </button>
              {relay.region && (
                <div style={{ display: "flex", justifyContent: "space-between", marginTop: 6, fontSize: 9, color: "var(--theme-text-muted, #5a5f6d)" }}>
                  <span>Last region</span>
                  <span>{relay.region}</span>
                </div>
              )}
            </div>
          )}

          {/* --- ACTIVATING state --- */}
          {relayLicensed && relayActivating && !relayActive && (
            <div style={{
              background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "10px 10px",
              border: "1px solid #d2992240", textAlign: "center",
            }}>
              <div style={{ fontSize: 10, color: "#d29922", fontWeight: 600 }}>
                Starting relay&hellip;
              </div>
            </div>
          )}

          {/* --- ERROR state --- */}
          {relayLicensed && relayError && !relayActive && !relayActivating && (
            <div style={{
              background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "8px 10px",
              border: "1px solid #da363340",
            }}>
              <div style={{ fontSize: 10, color: "#da3633", fontWeight: 600, marginBottom: 4 }}>
                Failed to start relay
              </div>
              <div style={{ fontSize: 9, color: "var(--theme-text-muted, #8b8f98)", marginBottom: 6 }}>
                {relayError}
              </div>
              <button onClick={handleRelayToggle} style={{
                width: "100%", padding: "5px 0", border: "1px solid var(--theme-border, #2a2d35)",
                borderRadius: 3, background: "var(--theme-panel, #20232b)", cursor: "pointer",
                color: "#5ba3f5", fontSize: 9, fontWeight: 600,
                fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
              }}>
                Retry
              </button>
            </div>
          )}

          {/* --- ACTIVE state --- */}
          {relayLicensed && relayActive && (
            <>
              <div style={{
                background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "8px 10px",
                border: "1px solid var(--theme-border, #1e2028)", marginBottom: 6,
              }}>
                <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 6 }}>
                  <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)" }}>Region</span>
                  <span style={{ fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontWeight: 600 }}>
                    {relay.region || "\u2014"}
                  </span>
                </div>
                <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 6 }}>
                  <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)" }}>Relay Latency</span>
                  <span style={{ fontSize: 10, color: "#2ea043", fontWeight: 600 }}>
                    {relay.latencyMs != null ? `${relay.latencyMs}ms` : "\u2014"}
                  </span>
                </div>
                {relay.graceRemainingSeconds != null && relay.graceRemainingSeconds > 0 && (
                  <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 6 }}>
                    <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)" }}>Grace Remaining</span>
                    <span style={{ fontSize: 10, color: "#a78bfa", fontWeight: 600 }}>
                      {formatTime(relay.graceRemainingSeconds)}
                    </span>
                  </div>
                )}
                <div style={{ display: "flex", justifyContent: "space-between" }}>
                  <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)" }}>Uptime</span>
                  <span style={{ fontSize: 10, color: "var(--theme-text, #e0e2e8)" }}>
                    {formatTime(relay.uptimeSec)}
                  </span>
                </div>
              </div>

              {/* Deactivate button */}
              <button onClick={handleRelayToggle} style={{
                width: "100%", padding: "5px 0", border: "1px solid var(--theme-border, #2a2d35)",
                borderRadius: 3, background: "var(--theme-panel, #20232b)", cursor: "pointer",
                color: "var(--theme-text-muted, #5a5f6d)", fontSize: 9, fontWeight: 500,
                fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                marginBottom: 6,
              }}>
                Deactivate Relay
              </button>

              {/* Time Bank — [PLACEHOLDER] not in IPC v1, needs control-plane quota */}
              <div style={{
                background: `linear-gradient(135deg, var(--theme-surface, #13151a) 0%, var(--theme-panel, #161820) 100%)`,
                borderRadius: 4, padding: "8px 10px",
                border: "1px solid var(--theme-border, #1e2028)", opacity: 0.6,
              }}>
                <div style={{ display: "flex", alignItems: "center", gap: 6, marginBottom: 6 }}>
                  <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)" }}>Time Bank</span>
                  <span style={{ fontSize: 8, color: "var(--theme-text-muted, #5a5f6d)", marginLeft: 2 }}>(pending)</span>
                  <span style={{ fontSize: 10, color: "#fbbf24", fontWeight: 600, marginLeft: "auto" }}>
                    {"\u2014"}
                  </span>
                </div>
                <div style={{ height: 3, background: "var(--theme-border, #1a1d23)", borderRadius: 2, overflow: "hidden" }}>
                  <div style={{
                    height: "100%", width: "0%",
                    background: "linear-gradient(90deg, #2d7aed, #5ba3f5)",
                    borderRadius: 2,
                  }} />
                </div>
              </div>
            </>
          )}
        </Section>

        {/* ----- FAILOVER ENGINE ----- */}
        <Section title="Failover Engine" icon="⚡" compact={isCompact}>
          <div style={{
            display: "flex", alignItems: "center", gap: 4, marginBottom: 8,
            padding: "6px 8px", background: "var(--theme-surface, #13151a)", borderRadius: 4,
            border: `1px solid ${healthColor}40`,
          }}>
            <div style={{
              width: 6, height: 6, borderRadius: "50%", background: healthColor,
              boxShadow: `0 0 6px ${healthColor}40`, flexShrink: 0,
            }} />
            <span style={{ fontSize: 10, color: healthColor, fontWeight: 600 }}>
              {healthLabel}
            </span>
            <span style={{ fontSize: 9, color: "var(--theme-text-muted, #5a5f6d)", marginLeft: "auto" }}>
              {engineState}
            </span>
          </div>

          <EngineStateChips activeState={engineState} compact={isUltraCompact} />

          <div style={{ fontSize: 9, color: "var(--theme-text-muted, #5a5f6d)", lineHeight: 1.6 }}>
            <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 2 }}>
              <span>Response Budget</span>
              <span style={{ color: "#2ea043", fontWeight: 600 }}>
                &lt; {failover.responseBudgetMs || 800}ms
              </span>
            </div>
            <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 2 }}>
              <span>Last Transition</span>
              <span style={{ color: "var(--theme-text-muted, #8b8f98)" }}>
                {failover.lastFailoverLabel || "\u2014"}
              </span>
            </div>
            <div style={{ display: "flex", justifyContent: "space-between" }}>
              <span>Total Transitions</span>
              <span style={{ color: "var(--theme-text-muted, #8b8f98)" }}>
                {failover.totalFailoversLabel || "\u2014"}
              </span>
            </div>
          </div>
        </Section>

        {/* ----- QUICK SETTINGS ----- */}
        <Section title="Quick Settings" icon="⚙" compact={isCompact}>
          {settings
            .filter((setting) => !["manual_override", "auto_scene_switch"].includes(setting.key))
            .map((setting) => (
            <ToggleRow
              key={setting.key}
              label={setting.label}
              value={setting.value}
              color={SETTING_COLORS[setting.key] || "#2d7aed"}
              dimmed={setting.value === null}
              onChange={(val) => handleSettingChange(setting.key, val)}
            />
          ))}
        </Section>

        {/* ----- EVENT LOG ----- */}
        <Section title="Event Log" icon="▤" compact={isCompact}
          badge={String(events.length)} badgeColor="var(--theme-surface, #3a3d45)">
          {events.map((e, i) => (
            <div key={e.id || i} style={{
              display: "flex", gap: 8, padding: "4px 0",
              borderBottom: i < events.length - 1 ? "1px solid var(--theme-border, #13151a)" : "none",
              animation: `slideIn 0.3s ease ${i * 0.05}s both`,
            }}>
              <span style={{
                fontSize: 9, color: "var(--theme-text-muted, #3a3d45)",
                fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)", flexShrink: 0,
              }}>
                {e.time}
              </span>
              <span style={{
                fontSize: 9, fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                color: e.type === "success" ? "#4ade80" :
                       e.type === "warning" ? "#fbbf24" :
                       e.type === "error"   ? "#da3633" : "var(--theme-text-muted, #6b7080)",
                wordBreak: "break-word",
              }}>{e.msg}</span>
            </div>
          ))}
        </Section>
      </div>

      {/* ================================================================= */}
      {/* FOOTER                                                            */}
      {/* ================================================================= */}
      <div style={{
        padding: isCompact ? "6px 10px" : "7px 12px", flexShrink: 0,
        borderTop: "1px solid var(--theme-border, #1a1d23)",
        background: "var(--theme-bg, #0c0e13)",
        display: "flex", alignItems: "center", justifyContent: "space-between",
        gap: 6, flexWrap: isUltraCompact ? "wrap" : "nowrap",
      }}>
        <span style={{
          fontSize: 8, color: "var(--theme-text-muted, #3a3d45)", letterSpacing: "0.06em",
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
        }}>
          TELEMY {version.toUpperCase?.() || version}
        </span>
        <div style={{ display: "flex", alignItems: "center", gap: 4 }}>
          <span style={{
            fontSize: 8, color: pipeColor,
            textShadow: `0 0 4px ${pipeColor}40`,
          }}>{"●"}</span>
          <span style={{ fontSize: 8, color: "var(--theme-text-muted, #3a3d45)" }}>{pipeLabel}</span>
        </div>
      </div>

      {/* Bridge unavailable indicator (dev/preview only) */}
      {!useBridge && (
        <div style={{
          position: "absolute", top: 6, right: 6,
          fontSize: 7, color: "#da3633", background: "#260d0d",
          padding: "1px 5px", borderRadius: 2, border: "1px solid #3a1a1a",
          letterSpacing: "0.06em", fontWeight: 700, opacity: 0.8,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
        }}>
          SIM
        </div>
      )}
    </div>
  );
}
