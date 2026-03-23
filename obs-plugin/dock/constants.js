// =============================================================================
// CONSTANTS & THEME
// =============================================================================

// Canonical engine states (STATE_MACHINE_v1.md)
export const ENGINE_STATES = [
  { id: "STUDIO",         short: "STU",  color: "#5ba3f5", bgActive: "#0d1a2e", borderActive: "#1a3a5a" },
  { id: "IRL_CONNECTING",  short: "CONN", color: "#fbbf24", bgActive: "#261e0d", borderActive: "#3a2a0d" },
  { id: "IRL_ACTIVE",      short: "ACTV", color: "#4ade80", bgActive: "#0d2618", borderActive: "#1a3a1a" },
  { id: "IRL_GRACE",       short: "GRC",  color: "#a78bfa", bgActive: "#1a0d26", borderActive: "#2a1a3a" },
  { id: "DEGRADED",        short: "DRGD", color: "#fbbf24", bgActive: "#261e0d", borderActive: "#3a2a0d" },
  { id: "FATAL",           short: "FATL", color: "#da3633", bgActive: "#260d0d", borderActive: "#3a1a1a" },
];

export const HEALTH_COLORS = {
  healthy:  "#2ea043",
  degraded: "#d29922",
  offline:  "#da3633",
};

export const SCENE_INTENT_COLORS = {
  LIVE:    { bg: "#1a3a1a", border: "#2ea043", text: "#4ade80" },
  BRB:     { bg: "#2a1a2a", border: "#8b5cf6", text: "#a78bfa" },
  HOLD:    { bg: "#3a2a1a", border: "#d29922", text: "#fbbf24" },
  OFFLINE: { bg: "#1a1a2a", border: "#4a4f5c", text: "#8b8f98" },
};

export const PIPE_STATUS_COLORS = {
  ok:       "#2ea043",
  degraded: "#d29922",
  down:     "#da3633",
};

// Per-key colors for settings toggles (bridge doesn't provide colors)
export const SETTING_COLORS = {
  auto_scene_switch:   "#2ea043",
  low_quality_fallback: "#d29922",
  manual_override:     "#5ba3f5",
  chat_bot:            "#8b8f98",
  alerts:              "#2d7aed",
};

// UI anti-mash timing (tuned for OBS dock responsiveness + IPC round-trip cadence)
export const UI_ACTION_COOLDOWNS_MS = {
  switchScene: 500,
  setMode: 500,
  setSetting: 350,
  autoSceneSwitch: 500,
};
export const AUTO_SWITCH_LOCK_TIMEOUT_MS = 1500;

export const OUTPUT_HEALTH_COLORS = {
  healthy:  "#2ea043",
  good:     "#8ac926",
  warning:  "#d29922",
  degraded: "#e85d04",
  critical: "#da3633",
};

export function getOutputHealthColor(currentKbps, maxObservedKbps) {
  if (!maxObservedKbps || maxObservedKbps <= 0 || !currentKbps) return OUTPUT_HEALTH_COLORS.critical;
  const pct = currentKbps / maxObservedKbps;
  if (pct >= 0.9) return OUTPUT_HEALTH_COLORS.healthy;
  if (pct >= 0.7) return OUTPUT_HEALTH_COLORS.good;
  if (pct >= 0.5) return OUTPUT_HEALTH_COLORS.warning;
  if (pct >= 0.3) return OUTPUT_HEALTH_COLORS.degraded;
  return OUTPUT_HEALTH_COLORS.critical;
}

export const CONNECTION_STATUS = {
  PROVISIONING: 'provisioning',
  READY: 'ready',
  LIVE: 'live',
  ERROR: 'error',
};

export const ACTION_BILLING_CHECKOUT = 'billing_checkout';

export const SCENE_LINK_STORAGE_KEY = "aegis.scene.intent.links.v1";
export const SCENE_LINK_NAME_STORAGE_KEY = "aegis.scene.intent.links.by_name.v1";
export const AUTO_SCENE_RULES_STORAGE_KEY = "aegis.auto.scene.rules.v2";
export const OUTPUT_CONFIG_STORAGE_KEY = "aegis.output.config.v1";
// AUTO_SWITCH_SOURCE_OPTIONS removed — auto-switch source now derived from relay.active
export const DEFAULT_AUTO_SCENE_RULES = [
  { id: "live_main", label: "Live - Main", intent: "LIVE", thresholdEnabled: false, thresholdMbps: null, isDefault: true, bgColor: "#2ea043" },
  { id: "low_bitrate_fallback", label: "Low Bitrate Fallback", intent: "HOLD", thresholdEnabled: true, thresholdMbps: 1.0, isDefault: false, bgColor: "#d29922" },
  { id: "brb_reconnecting", label: "BRB - Reconnecting", intent: "BRB", thresholdEnabled: true, thresholdMbps: 0.2, isDefault: false, bgColor: "#8b5cf6" },
  { id: "starting_soon", label: "Starting Soon", intent: "OFFLINE", thresholdEnabled: false, thresholdMbps: null, isDefault: false, bgColor: "#8b8f98" },
  { id: "ending", label: "Ending", intent: "OFFLINE", thresholdEnabled: false, thresholdMbps: null, isDefault: false, bgColor: "#8b8f98" },
];

export const RULE_BG_PRESETS = [
  { id: "live", color: "#2ea043", label: "Live Green" },
  { id: "hold", color: "#d29922", label: "Hold Amber" },
  { id: "brb", color: "#8b5cf6", label: "BRB Violet" },
  { id: "offline", color: "#8b8f98", label: "Offline Slate" },
  { id: "alert", color: "#da3633", label: "Alert Red" },
];

export const SCENE_PROFILE_NAME_HINTS = {
  live_main: ["main", "live - main", "live main", "live"],
  low_bitrate_fallback: ["low bitrate default scene", "low bitrate fallback", "low bitrate", "fallback", "low", "test"],
  brb_reconnecting: ["brb", "brb - reconnecting", "brb reconnecting", "reconnecting"],
  starting_soon: ["game audio", "starting soon", "starting"],
  ending: ["game audio", "ending", "end"],
};

// =============================================================================
// THEME DEFAULTS — Yami Grey palette (overridden by state.theme at runtime)
// =============================================================================

export const OBS_YAMI_GREY_DEFAULTS = {
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
export const SIM_SCENES = [
  { id: "scene_1", name: "Live - Main",           kind: null, intent: "live", index: 0 },
  { id: "scene_2", name: "Low Bitrate Fallback",   kind: null, intent: "hold", index: 1 },
  { id: "scene_3", name: "BRB - Reconnecting",     kind: null, intent: "brb",  index: 2 },
  { id: "scene_4", name: "Starting Soon",           kind: null, intent: null,   index: 3 },
  { id: "scene_5", name: "Ending",                  kind: null, intent: null,   index: 4 },
];

export const SIM_SETTING_DEFS = [
  { key: "auto_scene_switch",   label: "Auto Scene Switch" },
  { key: "low_quality_fallback", label: "Low Bitrate Failover" },
  { key: "manual_override",     label: "Manual Override" },
  { key: "chat_bot",            label: "Chat Bot Integration" },
  { key: "alerts",              label: "Alert on Disconnect" },
];

export const SIM_EVENTS = [
  { id: "e1", time: "01:04:07", tsUnixMs: Date.now() - 60000,  type: "success", msg: "Bitrate recovered \u2192 IRL_ACTIVE", source: "bridge" },
  { id: "e2", time: "01:03:42", tsUnixMs: Date.now() - 85000,  type: "warning", msg: "Low bitrate detected (intent BRB)",   source: "bridge" },
  { id: "e3", time: "01:03:40", tsUnixMs: Date.now() - 87000,  type: "warning", msg: "SIM 1 signal degraded",               source: "bridge" },
  { id: "e4", time: "00:58:12", tsUnixMs: Date.now() - 420000, type: "info",    msg: "Relay telemetry connected",           source: "ipc" },
  { id: "e5", time: "00:00:01", tsUnixMs: Date.now() - 3900000, type: "info",   msg: "Core connected",                      source: "ipc" },
];
