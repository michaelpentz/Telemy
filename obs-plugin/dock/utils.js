import { DEFAULT_AUTO_SCENE_RULES, RULE_BG_PRESETS, SCENE_PROFILE_NAME_HINTS, SCENE_LINK_STORAGE_KEY, SCENE_LINK_NAME_STORAGE_KEY, AUTO_SCENE_RULES_STORAGE_KEY, SCENE_INTENT_COLORS } from "./constants.js";

// =============================================================================
// UTILITIES
// =============================================================================

let _reqCounter = 0;
export function genRequestId() {
  return `dock_${Date.now()}_${++_reqCounter}`;
}

export function formatTime(s) {
  if (s == null || s < 0) return "\u2014";
  const sec = Math.floor(s);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const ss = sec % 60;
  return `${h.toString().padStart(2, "0")}:${m.toString().padStart(2, "0")}:${ss.toString().padStart(2, "0")}`;
}

export function parseHexColor(hex) {
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

export function toRgba(hex, alpha) {
  const rgb = parseHexColor(hex);
  if (!rgb) return `rgba(91,163,245,${alpha})`;
  return `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${alpha})`;
}

export function isLightColor(hex) {
  const rgb = parseHexColor(hex);
  if (!rgb) return false;
  const luminance = (0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b) / 255;
  return luminance > 0.6;
}

export function normalizeOptionalHexColor(value) {
  const rgb = parseHexColor(value);
  if (!rgb) return null;
  const toHex = (n) => n.toString(16).padStart(2, "0");
  return `#${toHex(rgb.r)}${toHex(rgb.g)}${toHex(rgb.b)}`;
}

export function getDefaultRuleBgColor(rule) {
  const id = String(rule?.id || "");
  const intent = String(rule?.intent || "").toUpperCase();
  if (id === "live_main" || intent === "LIVE") return "#2ea043";
  if (id === "low_bitrate_fallback" || intent === "HOLD") return "#d29922";
  if (id === "brb_reconnecting" || intent === "BRB") return "#8b5cf6";
  if (id === "starting_soon" || id === "ending" || intent === "OFFLINE") return "#8b8f98";
  return "#8b8f98";
}

export function normalizeIntent(intent) {
  if (!intent) return null;
  const upper = intent.toUpperCase();
  if (upper in SCENE_INTENT_COLORS) return upper;
  return null;
}

export function inferIntentFromName(name) {
  if (!name) return "OFFLINE";
  const lower = name.toLowerCase();
  if (lower.includes("live") || lower.includes("main")) return "LIVE";
  if (lower.includes("brb") || lower.includes("reconnect")) return "BRB";
  if (lower.includes("low") || lower.includes("fallback")) return "HOLD";
  return "OFFLINE";
}

export function normalizeSceneName(name) {
  return String(name || "").toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();
}

export function findBestSceneIdForRule(rule, sceneItems) {
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


export function loadSceneIntentLinks() {
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

export function loadSceneIntentLinkNames() {
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

export function findSceneIdByName(sceneName, sceneItems) {
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

export function normalizeLinkMap(raw) {
  if (!raw || typeof raw !== "object") return {};
  const out = {};
  Object.keys(raw).forEach((k) => {
    out[String(k)] = String(raw[k] || "");
  });
  return out;
}

// loadAutoSwitchSource removed — auto-switch source now derived from relay.active

export function loadAutoSceneRules() {
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

export function normalizeAutoSceneRulesValue(rules) {
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
      const explicitAliases = Array.isArray(r.chatAliases)
        ? Array.from(new Set(r.chatAliases.map((alias) => String(alias || "").trim().toLowerCase()).filter(Boolean))).slice(0, 6)
        : [];
      // Backfill default aliases for built-in rules that have none (e.g. loaded from old prefs)
      const defaultRule = explicitAliases.length === 0
        ? DEFAULT_AUTO_SCENE_RULES.find((d) => d.id === id)
        : null;
      const chatAliases = explicitAliases.length > 0 ? explicitAliases
        : (defaultRule && Array.isArray(defaultRule.chatAliases) ? defaultRule.chatAliases : []);
      const chatEnabled = r.chatEnabled != null ? r.chatEnabled !== false
        : (defaultRule ? defaultRule.chatEnabled !== false : true);
      return {
        id,
        label,
        intent,
        thresholdEnabled,
        thresholdMbps: Number.isFinite(threshold) && threshold >= 0 ? threshold : null,
        isDefault: !!r.isDefault,
        bgColor: normalizeOptionalHexColor(r.bgColor) || getDefaultRuleBgColor({ id, intent }),
        chatEnabled,
        chatAliases,
      };
    })
    .filter(Boolean);
  return normalized.length ? normalized : DEFAULT_AUTO_SCENE_RULES;
}

// --- CEF-safe clipboard copy (execCommand fallback for CEF panels) ---
export function cefCopyToClipboard(text) {
  // navigator.clipboard doesn't work in OBS CEF panels — use execCommand fallback
  var ta = document.createElement("textarea");
  ta.value = text;
  ta.style.position = "fixed";
  ta.style.left = "-9999px";
  ta.style.top = "-9999px";
  document.body.appendChild(ta);
  ta.select();
  document.execCommand("copy");
  document.body.removeChild(ta);
}

/** Known mobile carrier ASN org → short label */
var MOBILE_CARRIER_LABELS = {
  "t-mobile": "T-Mobile",
  "at&t": "AT&T",
  "verizon": "Verizon",
  "sprint": "Sprint",
  "vodafone": "Vodafone",
  "ee limited": "EE",
  "three": "Three",
  "o2": "O2",
  "telstra": "Telstra",
  "optus": "Optus",
  "rogers": "Rogers",
  "bell": "Bell",
  "telus": "TELUS",
  "orange": "Orange",
  "bouygues": "Bouygues",
  "sfr": "SFR",
  "swisscom": "Swisscom",
  "kddi": "KDDI",
  "softbank": "SoftBank",
  "ntt docomo": "Docomo",
  "jio": "Jio",
  "airtel": "Airtel",
};

/** Known ISP ASN org → short label (wired/ethernet connections) */
var ISP_LABELS = {
  "comcast": "Comcast",
  "charter": "Charter",
  "cox": "Cox",
  "centurylink": "CenturyLink",
  "spectrum": "Spectrum",
  "lumen": "Lumen",
  "frontier": "Frontier",
  "windstream": "Windstream",
  "mediacom": "Mediacom",
  "altice": "Altice",
  "optimum": "Optimum",
  "consolidated": "Consolidated",
  "google fiber": "Google Fiber",
  "sonic": "Sonic",
  "at&t internet": "AT&T Fiber",
  "verizon fios": "Fios",
  "bt group": "BT",
  "sky broadband": "Sky",
  "virgin media": "Virgin",
  "deutsche telekom": "Telekom",
  "telstra internet": "Telstra",
  "nbn": "NBN",
};

/** Parse ASN org name to a short carrier label and type */
export function classifyAsn(asnOrg) {
  if (!asnOrg) return null;
  var lower = asnOrg.toLowerCase();
  // Check ISPs first (more specific matches like "at&t internet" before "at&t")
  for (var key in ISP_LABELS) {
    if (lower.includes(key)) return { label: ISP_LABELS[key], type: "ethernet" };
  }
  // Check mobile carriers
  for (var key in MOBILE_CARRIER_LABELS) {
    if (lower.includes(key)) return { label: MOBILE_CARRIER_LABELS[key], type: "cellular" };
  }
  // Unknown ASN — use first word as label, assume cellular (most IRL links are)
  var first = asnOrg.split(/[,\s]+/)[0];
  var label = (first && first.length <= 12) ? first : asnOrg.slice(0, 12);
  return { label: label, type: "cellular" };
}

/** Kept for backward compat — delegates to classifyAsn */
export function shortCarrierLabel(asnOrg) {
  var result = classifyAsn(asnOrg);
  return result ? result.label : null;
}

/** Classify a link — uses ASN org when available, otherwise numbered fallback.
 *  linkIndex is the 0-based position in the links array. */
export function classifyLinkAddr(addr, asnOrg, linkIndex) {
  if (!addr) return { label: "Link", type: "unknown" };
  // Use ASN org if available (MaxMind on relay)
  if (asnOrg) {
    return classifyAsn(asnOrg);
  }
  // No ASN — use numbered labels (no IP heuristics, avoids leaking assumptions)
  var n = (linkIndex != null ? linkIndex : 0) + 1;
  return { label: "Link " + n, type: "unknown" };
}
