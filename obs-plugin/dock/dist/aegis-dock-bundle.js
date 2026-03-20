var AegisDock = (() => {
  var __require = /* @__PURE__ */ ((x) => typeof require !== "undefined" ? require : typeof Proxy !== "undefined" ? new Proxy(x, {
    get: (a, b) => (typeof require !== "undefined" ? require : a)[b]
  }) : x)(function(x) {
    if (typeof require !== "undefined") return require.apply(this, arguments);
    throw Error('Dynamic require of "' + x + '" is not supported');
  });

  // aegis-dock-entry.jsx
  var import_client = __require("react-dom/client");

  // aegis-dock.jsx
  var import_react7 = __require("react");

  // constants.js
  var ENGINE_STATES = [
    { id: "STUDIO", short: "STU", color: "#5ba3f5", bgActive: "#0d1a2e", borderActive: "#1a3a5a" },
    { id: "IRL_CONNECTING", short: "CONN", color: "#fbbf24", bgActive: "#261e0d", borderActive: "#3a2a0d" },
    { id: "IRL_ACTIVE", short: "ACTV", color: "#4ade80", bgActive: "#0d2618", borderActive: "#1a3a1a" },
    { id: "IRL_GRACE", short: "GRC", color: "#a78bfa", bgActive: "#1a0d26", borderActive: "#2a1a3a" },
    { id: "DEGRADED", short: "DRGD", color: "#fbbf24", bgActive: "#261e0d", borderActive: "#3a2a0d" },
    { id: "FATAL", short: "FATL", color: "#da3633", bgActive: "#260d0d", borderActive: "#3a1a1a" }
  ];
  var HEALTH_COLORS = {
    healthy: "#2ea043",
    degraded: "#d29922",
    offline: "#da3633"
  };
  var SCENE_INTENT_COLORS = {
    LIVE: { bg: "#1a3a1a", border: "#2ea043", text: "#4ade80" },
    BRB: { bg: "#2a1a2a", border: "#8b5cf6", text: "#a78bfa" },
    HOLD: { bg: "#3a2a1a", border: "#d29922", text: "#fbbf24" },
    OFFLINE: { bg: "#1a1a2a", border: "#4a4f5c", text: "#8b8f98" }
  };
  var PIPE_STATUS_COLORS = {
    ok: "#2ea043",
    degraded: "#d29922",
    down: "#da3633"
  };
  var SETTING_COLORS = {
    auto_scene_switch: "#2ea043",
    low_quality_fallback: "#d29922",
    manual_override: "#5ba3f5",
    chat_bot: "#8b8f98",
    alerts: "#2d7aed"
  };
  var UI_ACTION_COOLDOWNS_MS = {
    switchScene: 500,
    setMode: 500,
    setSetting: 350,
    autoSceneSwitch: 500
  };
  var AUTO_SWITCH_LOCK_TIMEOUT_MS = 1500;
  var OUTPUT_HEALTH_COLORS = {
    healthy: "#2ea043",
    good: "#8ac926",
    warning: "#d29922",
    degraded: "#e85d04",
    critical: "#da3633"
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
  var SCENE_LINK_STORAGE_KEY = "aegis.scene.intent.links.v1";
  var SCENE_LINK_NAME_STORAGE_KEY = "aegis.scene.intent.links.by_name.v1";
  var AUTO_SCENE_RULES_STORAGE_KEY = "aegis.auto.scene.rules.v2";
  var OUTPUT_CONFIG_STORAGE_KEY = "aegis.output.config.v1";
  var DEFAULT_AUTO_SCENE_RULES = [
    { id: "live_main", label: "Live - Main", intent: "LIVE", thresholdEnabled: false, thresholdMbps: null, isDefault: true, bgColor: "#2ea043" },
    { id: "low_bitrate_fallback", label: "Low Bitrate Fallback", intent: "HOLD", thresholdEnabled: true, thresholdMbps: 1, isDefault: false, bgColor: "#d29922" },
    { id: "brb_reconnecting", label: "BRB - Reconnecting", intent: "BRB", thresholdEnabled: true, thresholdMbps: 0.2, isDefault: false, bgColor: "#8b5cf6" },
    { id: "starting_soon", label: "Starting Soon", intent: "OFFLINE", thresholdEnabled: false, thresholdMbps: null, isDefault: false, bgColor: "#8b8f98" },
    { id: "ending", label: "Ending", intent: "OFFLINE", thresholdEnabled: false, thresholdMbps: null, isDefault: false, bgColor: "#8b8f98" }
  ];
  var RULE_BG_PRESETS = [
    { id: "live", color: "#2ea043", label: "Live Green" },
    { id: "hold", color: "#d29922", label: "Hold Amber" },
    { id: "brb", color: "#8b5cf6", label: "BRB Violet" },
    { id: "offline", color: "#8b8f98", label: "Offline Slate" },
    { id: "alert", color: "#da3633", label: "Alert Red" }
  ];
  var SCENE_PROFILE_NAME_HINTS = {
    live_main: ["main", "live - main", "live main", "live"],
    low_bitrate_fallback: ["low bitrate default scene", "low bitrate fallback", "low bitrate", "fallback", "low", "test"],
    brb_reconnecting: ["brb", "brb - reconnecting", "brb reconnecting", "reconnecting"],
    starting_soon: ["game audio", "starting soon", "starting"],
    ending: ["game audio", "ending", "end"]
  };
  var OBS_YAMI_GREY_DEFAULTS = {
    bg: "#1e1e1e",
    // QPalette::Window — main dock background
    surface: "#272727",
    // QPalette::Base — section headers, card backgrounds
    panel: "#2d2d2d",
    // QPalette::Button — slightly elevated panels
    text: "#e0e0e0",
    // QPalette::WindowText — primary text
    textMuted: "#909090",
    // ~60% text — secondary/dim labels
    accent: "#1473e6",
    // QPalette::Highlight — OBS brand blue
    border: "#383838",
    // derived — dividers, card outlines
    scrollbar: "#484848",
    // derived — scrollbar thumb
    fontFamily: ""
  };
  var SIM_SCENES = [
    { id: "scene_1", name: "Live - Main", kind: null, intent: "live", index: 0 },
    { id: "scene_2", name: "Low Bitrate Fallback", kind: null, intent: "hold", index: 1 },
    { id: "scene_3", name: "BRB - Reconnecting", kind: null, intent: "brb", index: 2 },
    { id: "scene_4", name: "Starting Soon", kind: null, intent: null, index: 3 },
    { id: "scene_5", name: "Ending", kind: null, intent: null, index: 4 }
  ];
  var SIM_SETTING_DEFS = [
    { key: "auto_scene_switch", label: "Auto Scene Switch" },
    { key: "low_quality_fallback", label: "Low Bitrate Failover" },
    { key: "manual_override", label: "Manual Override" },
    { key: "chat_bot", label: "Chat Bot Integration" },
    { key: "alerts", label: "Alert on Disconnect" }
  ];
  var SIM_EVENTS = [
    { id: "e1", time: "01:04:07", tsUnixMs: Date.now() - 6e4, type: "success", msg: "Bitrate recovered \u2192 IRL_ACTIVE", source: "bridge" },
    { id: "e2", time: "01:03:42", tsUnixMs: Date.now() - 85e3, type: "warning", msg: "Low bitrate detected (intent BRB)", source: "bridge" },
    { id: "e3", time: "01:03:40", tsUnixMs: Date.now() - 87e3, type: "warning", msg: "SIM 1 signal degraded", source: "bridge" },
    { id: "e4", time: "00:58:12", tsUnixMs: Date.now() - 42e4, type: "info", msg: "Relay telemetry connected", source: "ipc" },
    { id: "e5", time: "00:00:01", tsUnixMs: Date.now() - 39e5, type: "info", msg: "Core connected", source: "ipc" }
  ];

  // utils.js
  var _reqCounter = 0;
  function genRequestId() {
    return `dock_${Date.now()}_${++_reqCounter}`;
  }
  function formatTime(s) {
    if (s == null || s < 0) return "\u2014";
    const sec = Math.floor(s);
    const h = Math.floor(sec / 3600);
    const m = Math.floor(sec % 3600 / 60);
    const ss = sec % 60;
    return `${h.toString().padStart(2, "0")}:${m.toString().padStart(2, "0")}:${ss.toString().padStart(2, "0")}`;
  }
  function parseHexColor(hex) {
    const raw = String(hex || "").trim();
    const cleaned = raw.startsWith("#") ? raw.slice(1) : raw;
    if (!/^[0-9a-fA-F]{3}$|^[0-9a-fA-F]{6}$/.test(cleaned)) return null;
    const normalized = cleaned.length === 3 ? cleaned.split("").map((c) => c + c).join("") : cleaned;
    return {
      r: parseInt(normalized.slice(0, 2), 16),
      g: parseInt(normalized.slice(2, 4), 16),
      b: parseInt(normalized.slice(4, 6), 16)
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
    const normalized = rules.map((r, idx) => {
      if (!r || typeof r !== "object") return null;
      const id = String(r.id || `rule_${idx}`);
      const label = String(r.label || `Rule ${idx + 1}`).slice(0, 40);
      const intent = normalizeIntent(r.intent) || "HOLD";
      const threshold = r.thresholdMbps === "" || r.thresholdMbps == null ? null : Number(r.thresholdMbps);
      const thresholdEnabled = typeof r.thresholdEnabled === "boolean" ? r.thresholdEnabled : Number.isFinite(threshold) && threshold >= 0;
      return {
        id,
        label,
        intent,
        thresholdEnabled,
        thresholdMbps: Number.isFinite(threshold) && threshold >= 0 ? threshold : null,
        isDefault: !!r.isDefault,
        bgColor: normalizeOptionalHexColor(r.bgColor) || getDefaultRuleBgColor({ id, intent })
      };
    }).filter(Boolean);
    return normalized.length ? normalized : DEFAULT_AUTO_SCENE_RULES;
  }
  function cefCopyToClipboard(text) {
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
    "airtel": "Airtel"
  };
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
    "nbn": "NBN"
  };
  function classifyAsn(asnOrg) {
    if (!asnOrg) return null;
    var lower = asnOrg.toLowerCase();
    for (var key in ISP_LABELS) {
      if (lower.includes(key)) return { label: ISP_LABELS[key], type: "ethernet" };
    }
    for (var key in MOBILE_CARRIER_LABELS) {
      if (lower.includes(key)) return { label: MOBILE_CARRIER_LABELS[key], type: "cellular" };
    }
    var first = asnOrg.split(/[,\s]+/)[0];
    var label = first && first.length <= 12 ? first : asnOrg.slice(0, 12);
    return { label, type: "cellular" };
  }
  function classifyLinkAddr(addr, asnOrg, linkIndex) {
    if (!addr) return { label: "Link", type: "unknown" };
    if (asnOrg) {
      return classifyAsn(asnOrg);
    }
    var n = (linkIndex != null ? linkIndex : 0) + 1;
    return { label: "Link " + n, type: "unknown" };
  }

  // css.js
  function getDockCss(theme) {
    return `
  /* === OBS CEF host sizing \u2014 ensures height:100% propagates to the component === */
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
  @keyframes provisionPulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
  }
  @keyframes dotBlink {
    0%, 100% { opacity: 0.2; }
    50% { opacity: 1; }
  }
  .aegis-dock-scroll::-webkit-scrollbar { width: 3px; }
  .aegis-dock-scroll::-webkit-scrollbar-track { background: transparent; }
  .aegis-dock-scroll::-webkit-scrollbar-thumb { background: ${theme.scrollbar}; border-radius: 2px; }
  .aegis-dock-scroll::-webkit-scrollbar-thumb:hover { background: ${theme.border}; border-radius: 2px; }
`;
  }

  // hooks.js
  var import_react = __require("react");
  function useAnimatedValue(target, duration = 800) {
    const [value, setValue] = (0, import_react.useState)(target);
    const ref = (0, import_react.useRef)(target);
    (0, import_react.useEffect)(() => {
      const start = ref.current;
      const diff = target - start;
      if (Math.abs(diff) < 0.5) {
        ref.current = target;
        setValue(target);
        return;
      }
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
    const maxMapRef = (0, import_react.useRef)({});
    const resultRef = (0, import_react.useRef)({ maxMap: {}, sectionMax: 0 });
    const maxMap = maxMapRef.current;
    let sectionMax = 0;
    if (Array.isArray(outputItems)) {
      outputItems.forEach((item) => {
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
    const [mode, setMode] = (0, import_react.useState)("regular");
    (0, import_react.useEffect)(() => {
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

  // use-dock-state.js
  var import_react2 = __require("react");
  function useDockState() {
    const [state, setState] = (0, import_react2.useState)(null);
    const [bridgeAvailable, setBridgeAvailable] = (0, import_react2.useState)(false);
    const actionMapRef = (0, import_react2.useRef)({});
    (0, import_react2.useEffect)(() => {
      let didInit = false;
      let unsub;
      let poll;
      let fastPoll;
      let earlyRefresh;
      let statusRequest;
      let sceneRetry;
      let sceneRetry2;
      const stateEvents = [
        "aegis:dock:native-ready",
        "aegis:dock:host-fallback",
        "aegis:dock:scene-snapshot",
        "aegis:dock:scene-snapshot-json",
        "aegis:dock:current-scene",
        "aegis:dock:pipe-status",
        "aegis:dock:scene-switch-completed",
        "aegis:dock:action-native-result"
      ];
      let refresh = () => {
      };
      const initNativeBridge = () => {
        if (didInit) return true;
        const native = window.aegisDockNative;
        if (!native || typeof native.getState !== "function") return false;
        didInit = true;
        setBridgeAvailable(true);
        refresh = () => {
          try {
            setState(native.getState());
          } catch (_) {
          }
        };
        try {
          setState(native.getState());
        } catch (_) {
        }
        stateEvents.forEach((e) => window.addEventListener(e, refresh));
        const host = window.aegisDockHost;
        if (host && typeof host.subscribe === "function") {
          unsub = host.subscribe(() => refresh());
        }
        poll = setInterval(refresh, 2e3);
        fastPoll = setInterval(refresh, 250);
        setTimeout(() => {
          if (fastPoll) {
            clearInterval(fastPoll);
            fastPoll = null;
          }
        }, 6e3);
        earlyRefresh = setTimeout(() => {
          try {
            setState(native.getState());
          } catch (_) {
          }
        }, 150);
        statusRequest = setTimeout(() => {
          try {
            native.sendDockAction({ type: "request_status", requestId: genRequestId() });
          } catch (_) {
          }
        }, 400);
        sceneRetry = setTimeout(() => {
          try {
            const s = native.getState();
            const items = s && s.scenes && Array.isArray(s.scenes.items) ? s.scenes.items : [];
            if (items.length === 0) {
              if (typeof native.replayLastSceneSnapshot === "function") {
                native.replayLastSceneSnapshot();
                refresh();
              }
              native.sendDockAction({ type: "request_scene_snapshot", requestId: genRequestId() });
            }
          } catch (_) {
          }
        }, 1e3);
        sceneRetry2 = setTimeout(() => {
          try {
            const s = native.getState();
            const items = s && s.scenes && Array.isArray(s.scenes.items) ? s.scenes.items : [];
            if (items.length === 0) {
              if (typeof native.replayLastSceneSnapshot === "function") {
                native.replayLastSceneSnapshot();
                refresh();
              }
            }
          } catch (_) {
          }
        }, 3e3);
        return true;
      };
      initNativeBridge();
      const retry = setInterval(() => {
        if (initNativeBridge()) clearInterval(retry);
      }, 250);
      return () => {
        clearInterval(retry);
        stateEvents.forEach((e) => window.removeEventListener(e, refresh));
        if (unsub) unsub();
        if (poll) clearInterval(poll);
        if (fastPoll) clearInterval(fastPoll);
        if (earlyRefresh) clearTimeout(earlyRefresh);
        if (statusRequest) clearTimeout(statusRequest);
        if (sceneRetry) clearTimeout(sceneRetry);
        if (sceneRetry2) clearTimeout(sceneRetry2);
      };
    }, []);
    (0, import_react2.useEffect)(() => {
      const handler = (e) => {
        const result = e.detail;
        if (!result?.requestId) return;
        const entry = actionMapRef.current[result.requestId];
        if (entry) {
          entry.status = result.status;
          entry.ok = result.ok;
          entry.error = result.error;
          if (result.status === "completed" || result.status === "failed" || result.status === "rejected") {
            setTimeout(() => {
              delete actionMapRef.current[result.requestId];
            }, 3e3);
          }
        }
      };
      window.addEventListener("aegis:dock:action-native-result", handler);
      return () => window.removeEventListener("aegis:dock:action-native-result", handler);
    }, []);
    const sendAction = (0, import_react2.useCallback)((action) => {
      const native = window.aegisDockNative;
      if (!native || typeof native.sendDockAction !== "function") {
        console.log("[aegis-dock] sendDockAction (no native):", action);
        return null;
      }
      if (!action.requestId) action.requestId = genRequestId();
      actionMapRef.current[action.requestId] = {
        type: action.type,
        status: "optimistic",
        ts: Date.now()
      };
      const result = native.sendDockAction(action);
      try {
        setState(native.getState());
      } catch (_) {
      }
      return result;
    }, []);
    return { state, sendAction, bridgeAvailable };
  }

  // use-simulated-state.js
  var import_react3 = __require("react");
  function useSimulatedState() {
    const [mode, setMode] = (0, import_react3.useState)("studio");
    const [simRelayActive, setSimRelayActive] = (0, import_react3.useState)(false);
    const [simRelayData, setSimRelayData] = (0, import_react3.useState)({
      relayHostname: "byor.telemy.test",
      ingestUrl: "srtla://byor.telemy.test:5000",
      pairToken: "ABCD-1234-EFGH",
      region: "custom",
      byorEnabled: true,
      byorRelayHost: "byor.telemy.test",
      byorRelayPort: 5e3,
      byorStreamId: "live/custom"
    });
    const [activeSceneId, setActiveSceneId] = (0, import_react3.useState)("scene_1");
    const [pendingSceneId, setPendingSceneId] = (0, import_react3.useState)(null);
    const [elapsed, setElapsed] = (0, import_react3.useState)(3847);
    const [sim1, setSim1] = (0, import_react3.useState)(4200);
    const [sim2, setSim2] = (0, import_react3.useState)(2800);
    const [settingValues, setSettingValues] = (0, import_react3.useState)({
      auto_scene_switch: true,
      low_quality_fallback: true,
      manual_override: false,
      chat_bot: null,
      alerts: true
    });
    (0, import_react3.useEffect)(() => {
      const iv = setInterval(() => {
        setSim1((prev) => Math.max(500, Math.min(6e3, prev + (Math.random() - 0.48) * 800)));
        setSim2((prev) => Math.max(200, Math.min(4e3, prev + (Math.random() - 0.5) * 600)));
        setElapsed((prev) => prev + 3);
      }, 3e3);
      return () => clearInterval(iv);
    }, []);
    const state = (0, import_react3.useMemo)(() => ({
      header: {
        title: "AEGIS",
        subtitle: "OBS + Core IPC Dock",
        mode,
        modes: ["studio", "irl"],
        version: "v0.0.5"
      },
      live: {
        isLive: true,
        elapsedSec: elapsed
      },
      scenes: {
        items: SIM_SCENES,
        activeSceneId,
        pendingSceneId,
        autoSwitchArmed: !settingValues.manual_override
      },
      connections: {
        items: [
          { name: "SIM 1 \u2014 T-Mobile", type: "5G", signal: 4, bitrate: sim1, status: "connected" },
          { name: "SIM 2 \u2014 Verizon", type: "LTE", signal: 3, bitrate: sim2, status: "connected" },
          { name: "WiFi", type: "802.11ac", signal: 0, bitrate: 0, status: "disconnected" }
        ]
      },
      relay_connections: [
        {
          id: "sim-byor-1",
          name: "Main Cam \u2192 My VPS",
          type: "byor",
          status: "connected",
          relay_host_masked: "my-relay.e***.com",
          relay_host: "my-relay.example.com",
          relay_port: 5e3,
          stream_id: "live/stream123",
          stats: { bitrate_kbps: Math.round(sim1 * 0.9), rtt_ms: 45, available: true },
          per_link: { available: false }
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
              { carrier: "Cox", bitrate_kbps: Math.round(sim2 * 0.58), rtt_ms: 28 },
              { carrier: "T-Mobile", bitrate_kbps: Math.round(sim2 * 0.42), rtt_ms: 38 }
            ]
          }
        },
        {
          id: "sim-byor-2",
          name: "Backup \u2192 BYOR EU",
          type: "byor",
          status: "idle",
          relay_host_masked: "eu-relay.e***.com",
          relay_host: "eu-relay.example.com",
          relay_port: 5e3,
          stream_id: "",
          stats: { bitrate_kbps: 0, rtt_ms: 0, available: false },
          per_link: { available: false }
        }
      ],
      bitrate: {
        bondedKbps: sim1 + sim2,
        relayBondedKbps: sim1 + sim2,
        maxPerLinkKbps: 6e3,
        maxBondedKbps: 12e3,
        lowThresholdMbps: 1.5,
        brbThresholdMbps: 0.5,
        outputs: [
          { platform: "Twitch", kbps: Math.max(800, Math.floor((sim1 + sim2) * 0.55)), status: "active" },
          { platform: "YouTube", kbps: Math.max(600, Math.floor((sim1 + sim2) * 0.45)), status: "active" },
          { platform: "Kick", kbps: Math.max(300, Math.floor((sim1 + sim2) * 0.25)), status: simRelayActive ? "active" : "idle" }
        ]
      },
      outputs: { groups: [], hidden: [] },
      relay: {
        licensed: true,
        active: simRelayActive,
        enabled: simRelayActive,
        // backward compat
        status: simRelayActive ? "active" : "inactive",
        byorEnabled: simRelayData.byorEnabled === true,
        byorRelayHost: simRelayData.byorRelayHost || "",
        byorRelayPort: simRelayData.byorRelayPort || 5e3,
        byorStreamId: simRelayData.byorStreamId || "",
        region: simRelayActive ? simRelayData.region || "us-east-1" : "us-east-1",
        latencyMs: simRelayActive ? 42 : null,
        uptimeSec: simRelayActive ? elapsed : 0,
        graceRemainingSeconds: null,
        relayHostname: simRelayActive ? simRelayData.relayHostname || null : null,
        ingestUrl: simRelayActive ? simRelayData.ingestUrl || null : null,
        pairToken: simRelayActive ? simRelayData.pairToken || null : null,
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
        recvRateMbps: simRelayActive ? (sim1 + sim2) / 1e3 : null,
        bandwidthMbps: simRelayActive ? 12 : null,
        uptimeSeconds: simRelayActive ? elapsed : 0,
        // Per-link simulated data
        perLinkAvailable: simRelayActive,
        connCount: simRelayActive ? 2 : 0,
        links: simRelayActive ? [
          { addr: "192.168.1.105:45032", bytes: Math.floor(sim1 * elapsed * 0.125), pkts: Math.floor(sim1 * elapsed * 0.125 / 1350), sharePct: sim1 / (sim1 + sim2) * 100, lastMsAgo: 12, uptimeS: elapsed },
          { addr: "198.51.100.99:38201", bytes: Math.floor(sim2 * elapsed * 0.125), pkts: Math.floor(sim2 * elapsed * 0.125 / 1350), sharePct: sim2 / (sim1 + sim2) * 100, lastMsAgo: 8, uptimeS: elapsed }
        ] : []
      },
      auth: {
        authenticated: true,
        hasTokens: true,
        user: {
          id: "sim-user",
          email: "preview@telemyapp.test",
          display_name: "Preview Operator"
        },
        entitlement: {
          relay_access_status: "enabled",
          reason_code: "",
          plan_tier: "starter",
          plan_status: "active",
          max_concurrent_conns: 1,
          active_managed_conns: 1
        },
        usage: {
          included_seconds: 0,
          consumed_seconds: 0,
          remaining_seconds: 0,
          overage_seconds: 0
        },
        activeRelay: null,
        login: { pending: false, poll_interval_seconds: 3 },
        lastErrorCode: null,
        lastErrorMessage: null
      },
      failover: {
        health: "healthy",
        state: simRelayActive ? "IRL_ACTIVE" : "STUDIO",
        states: ENGINE_STATES.map((s) => s.id),
        responseBudgetMs: 800,
        lastFailoverLabel: null,
        totalFailoversLabel: null
      },
      settings: {
        items: SIM_SETTING_DEFS.map((d) => ({
          ...d,
          value: settingValues[d.key] ?? null
        }))
      },
      events: SIM_EVENTS,
      pipe: { status: "ok", label: "IPC: OK" },
      meta: {
        coreVersion: null,
        pluginVersion: null,
        lastPongUnixMs: null,
        lastHelloAckUnixMs: null
      },
      theme: OBS_YAMI_GREY_DEFAULTS
    }), [mode, elapsed, activeSceneId, pendingSceneId, sim1, sim2, settingValues, simRelayActive, simRelayData]);
    const sendAction = (0, import_react3.useCallback)((action) => {
      switch (action.type) {
        case "switch_scene": {
          const targetId = action.sceneId || SIM_SCENES.find((s) => s.name === action.sceneName)?.id;
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
          setSettingValues((prev) => ({ ...prev, [action.key]: action.value }));
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
              byorRelayPort: 5e3,
              byorStreamId: "live/custom"
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
            byorRelayPort: action.byor_relay_port != null ? Number(action.byor_relay_port) || 5e3 : prev.byorRelayPort,
            byorStreamId: action.byor_stream_id != null ? String(action.byor_stream_id) : prev.byorStreamId,
            relayHostname: action.byor_relay_host != null ? String(action.byor_relay_host) : prev.relayHostname,
            ingestUrl: action.byor_relay_host != null ? `srtla://${String(action.byor_relay_host)}:${action.byor_relay_port != null ? Number(action.byor_relay_port) || 5e3 : prev.byorRelayPort || 5e3}` : prev.ingestUrl
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

  // ui-components.jsx
  var import_react4 = __require("react");
  var import_jsx_runtime = __require("react/jsx-runtime");
  function Section({ title, icon, badge, badgeColor, defaultOpen = false, compact = false, children, dragHandle }) {
    const [open, setOpen] = (0, import_react4.useState)(defaultOpen);
    return /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("div", { style: {
      borderBottom: "1px solid var(--theme-border, #1a1d23)",
      background: open ? "rgba(128,128,128,0.04)" : "transparent",
      transition: "background 0.2s ease"
    }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("div", { style: { display: "flex", alignItems: "stretch" }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime.jsxs)(
          "button",
          {
            onClick: () => setOpen(!open),
            style: {
              flex: 1,
              minWidth: 0,
              display: "flex",
              alignItems: "center",
              gap: 8,
              padding: compact ? "9px 10px" : "10px 12px",
              border: "none",
              background: "none",
              color: "var(--theme-text-muted, #c8ccd4)",
              cursor: "pointer",
              fontSize: compact ? 10 : 11,
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              fontWeight: 600,
              letterSpacing: "0.08em",
              textTransform: "uppercase",
              transition: "color 0.15s ease"
            },
            onMouseEnter: (e) => e.currentTarget.style.color = "var(--theme-text, #fff)",
            onMouseLeave: (e) => e.currentTarget.style.color = "var(--theme-text-muted, #c8ccd4)",
            children: [
              /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: { fontSize: 13, opacity: 0.6, lineHeight: 1 }, children: icon }),
              /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: { flex: 1, textAlign: "left" }, children: title }),
              badge != null && /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: {
                background: badgeColor || "#2d7aed",
                color: "var(--theme-bg, #fff)",
                fontSize: compact ? 7 : 8,
                fontWeight: 700,
                padding: compact ? "1px 4px" : "2px 6px",
                borderRadius: 3,
                letterSpacing: "0.04em",
                fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                maxWidth: compact ? 64 : 92,
                overflow: "hidden",
                textOverflow: "ellipsis",
                whiteSpace: "nowrap"
              }, children: badge }),
              /* @__PURE__ */ (0, import_jsx_runtime.jsx)(
                "svg",
                {
                  width: "10",
                  height: "10",
                  viewBox: "0 0 10 10",
                  style: {
                    transform: open ? "rotate(0deg)" : "rotate(-90deg)",
                    transition: "transform 0.2s ease",
                    opacity: 0.4
                  },
                  children: /* @__PURE__ */ (0, import_jsx_runtime.jsx)(
                    "path",
                    {
                      d: "M2 3.5L5 6.5L8 3.5",
                      stroke: "currentColor",
                      strokeWidth: "1.5",
                      fill: "none",
                      strokeLinecap: "round",
                      strokeLinejoin: "round"
                    }
                  )
                }
              )
            ]
          }
        ),
        dragHandle
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: {
        maxHeight: open ? 800 : 0,
        overflow: "hidden",
        transition: "max-height 0.3s cubic-bezier(0.4,0,0.2,1)",
        opacity: open ? 1 : 0
      }, children: /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: { padding: compact ? "2px 10px 10px" : "2px 12px 12px" }, children }) })
    ] });
  }
  function StatusDot({ color, pulse }) {
    return /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("span", { style: { position: "relative", display: "inline-flex", alignItems: "center", justifyContent: "center" }, children: [
      pulse && /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: {
        position: "absolute",
        width: 10,
        height: 10,
        borderRadius: "50%",
        background: color,
        opacity: 0.3,
        animation: "pulse 2s ease-in-out infinite"
      } }),
      /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: {
        width: 6,
        height: 6,
        borderRadius: "50%",
        background: color,
        boxShadow: `0 0 6px ${color}40`
      } })
    ] });
  }
  function BitrateBar({ value, max, color, label, suffix }) {
    const pct = max > 0 ? Math.min(value / max * 100, 100) : 0;
    return /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("div", { style: { marginBottom: 6 }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("div", { style: { display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 3 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("span", { style: { fontSize: 10, color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)", display: "inline-flex", alignItems: "center" }, children: [
          label,
          suffix
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: { fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)", fontWeight: 600 }, children: value >= 1e3 ? (value / 1e3).toFixed(1) + " Mbps" : Math.round(value) + " kbps" })
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: { height: 4, background: "var(--theme-surface, #1a1d23)", borderRadius: 2, overflow: "hidden" }, children: /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: {
        height: "100%",
        width: `${pct}%`,
        background: `linear-gradient(90deg, ${color}, ${color}cc)`,
        borderRadius: 2,
        transition: "width 0.6s cubic-bezier(0.4,0,0.2,1)",
        boxShadow: `0 0 8px ${color}30`
      } }) })
    ] });
  }
  function ToggleRow({ label, value, color, dimmed, onChange }) {
    const isOn = !!value;
    const isDimmed = dimmed || value === null;
    return /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("div", { style: {
      display: "flex",
      alignItems: "center",
      justifyContent: "space-between",
      padding: "6px 0",
      borderBottom: "1px solid var(--theme-border, #13151a)",
      opacity: isDimmed ? 0.45 : 1
    }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: {
        fontSize: 10,
        color: isOn ? "var(--theme-text, #c8ccd4)" : "var(--theme-text-muted, #5a5f6d)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: label }),
      /* @__PURE__ */ (0, import_jsx_runtime.jsx)("button", { onClick: () => {
        if (!isDimmed && onChange) onChange(!isOn);
      }, style: {
        width: 32,
        height: 16,
        borderRadius: 8,
        border: "none",
        cursor: isDimmed ? "not-allowed" : "pointer",
        background: isOn ? color || "var(--theme-accent, #2d7aed)" : "var(--theme-border, #2a2d35)",
        position: "relative",
        transition: "background 0.2s ease",
        flexShrink: 0
      }, children: /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: {
        width: 12,
        height: 12,
        borderRadius: "50%",
        background: "var(--theme-text, #fff)",
        position: "absolute",
        top: 2,
        left: isOn ? 18 : 2,
        transition: "left 0.2s ease",
        boxShadow: "0 1px 3px rgba(0,0,0,0.3)"
      } }) })
    ] });
  }
  function EngineStateChips({ activeState, compact = false }) {
    return /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: {
      display: "grid",
      gridTemplateColumns: compact ? "1fr 1fr" : "1fr 1fr 1fr",
      gap: 3,
      marginBottom: 8
    }, children: ENGINE_STATES.map((es) => {
      const isActive = activeState === es.id;
      return /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: {
        height: compact ? 20 : 22,
        borderRadius: 3,
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        fontSize: compact ? 6 : 7,
        fontWeight: 700,
        letterSpacing: "0.04em",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        background: isActive ? es.bgActive : "var(--theme-surface, #13151a)",
        border: `1px solid ${isActive ? es.borderActive : "var(--theme-border, #2a2d35)"}`,
        color: isActive ? es.color : "var(--theme-text-muted, #5a5f6d)",
        transition: "all 0.25s ease",
        boxShadow: isActive ? `0 0 8px ${es.color}15` : "none"
      }, children: es.short }, es.id);
    }) });
  }
  function ConnectionTypeBadge({ type }) {
    const isByor = type === "byor";
    return /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: {
      fontSize: 8,
      fontWeight: 700,
      letterSpacing: "0.06em",
      background: isByor ? "#1a3a5a" : "#1a3a2a",
      color: isByor ? "#5ba3f5" : "#4ade80",
      padding: "2px 5px",
      borderRadius: 2,
      flexShrink: 0,
      border: isByor ? "1px solid #2d7aed30" : "1px solid #2ea04330",
      fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
    }, children: isByor ? "BYOR" : "MGD" });
  }
  function SecretField({ label, value, copyValue }) {
    const [revealed, setRevealed] = (0, import_react4.useState)(false);
    const hasValue = !!value;
    const displayValue = revealed ? value : hasValue ? "\u2022".repeat(Math.min(String(value).length, 22)) : "\u2014";
    return /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("div", { style: { marginBottom: 5 }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime.jsx)("div", { style: {
        fontSize: 8,
        color: "var(--theme-text-muted, #8b8f98)",
        textTransform: "uppercase",
        letterSpacing: "0.04em",
        marginBottom: 2,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: label }),
      /* @__PURE__ */ (0, import_jsx_runtime.jsxs)("div", { style: { display: "flex", alignItems: "center", gap: 4 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime.jsx)("span", { style: {
          flex: 1,
          fontSize: 9,
          color: hasValue ? "var(--theme-text, #e0e2e8)" : "var(--theme-text-muted, #5a5f6d)",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          overflow: "hidden",
          textOverflow: "ellipsis",
          whiteSpace: "nowrap"
        }, children: displayValue }),
        hasValue && /* @__PURE__ */ (0, import_jsx_runtime.jsx)(
          "button",
          {
            onClick: () => setRevealed(!revealed),
            style: {
              border: "none",
              background: "none",
              padding: "0 2px",
              color: "var(--theme-text-muted, #4a4f5c)",
              fontSize: 8,
              cursor: "pointer",
              flexShrink: 0,
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
            },
            children: revealed ? "hide" : "show"
          }
        ),
        hasValue && /* @__PURE__ */ (0, import_jsx_runtime.jsx)(
          "button",
          {
            onClick: () => cefCopyToClipboard(copyValue || value),
            style: {
              border: "none",
              background: "none",
              padding: "0 2px",
              color: "var(--theme-text-muted, #4a4f5c)",
              fontSize: 9,
              cursor: "pointer",
              flexShrink: 0,
              lineHeight: 1
            },
            title: "Copy",
            children: "\u29C9"
          }
        )
      ] })
    ] });
  }

  // scene-components.jsx
  var import_jsx_runtime2 = __require("react/jsx-runtime");

  // encoder-components.jsx
  var import_react5 = __require("react");
  var import_jsx_runtime3 = __require("react/jsx-runtime");
  function OutputBar({ name, bitrateKbps, fps, dropPct, active, maxBitrate, compact = false }) {
    const healthColor = getOutputHealthColor(bitrateKbps, maxBitrate);
    const animBitrate = useAnimatedValue(active ? bitrateKbps || 0 : 0, 600);
    const pct = maxBitrate > 0 ? Math.min(animBitrate / maxBitrate * 100, 100) : 0;
    const inactive = !active;
    return /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: { marginBottom: compact ? 6 : 8, opacity: inactive ? 0.4 : 1, transition: "opacity 0.3s ease" }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: {
        display: "flex",
        justifyContent: "space-between",
        alignItems: "baseline",
        marginBottom: 3,
        fontSize: compact ? 9 : 10,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("span", { style: {
          color: "var(--theme-text, #e0e2e8)",
          fontWeight: 600,
          overflow: "hidden",
          textOverflow: "ellipsis",
          whiteSpace: "nowrap",
          flex: 1,
          marginRight: 8
        }, children: name || "Output" }),
        /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("span", { style: { color: "var(--theme-text-muted, #8b8f98)", flexShrink: 0, whiteSpace: "nowrap" }, children: inactive ? "\u2014" : /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)(import_jsx_runtime3.Fragment, { children: [
          bitrateKbps != null ? `${(bitrateKbps / 1e3).toFixed(1)} Mbps` : "\u2014",
          "  ",
          fps != null ? `${Math.round(fps)}fps` : "",
          "  ",
          dropPct != null ? `${dropPct.toFixed(2)}%` : ""
        ] }) })
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: {
        height: compact ? 3 : 4,
        background: "var(--theme-border, #2a2d35)",
        borderRadius: 2,
        overflow: "hidden"
      }, children: /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: {
        height: "100%",
        width: `${pct}%`,
        background: healthColor,
        borderRadius: 2,
        transition: "width 0.6s cubic-bezier(0.4,0,0.2,1), background 0.4s ease",
        boxShadow: inactive ? "none" : `0 0 4px ${healthColor}30`
      } }) })
    ] });
  }
  function EncoderGroupHeader({ name, resolution, totalBitrateKbps, avgLagMs, compact = false }) {
    if (name === "Ungrouped") return null;
    return /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: { marginTop: 8, marginBottom: 6 }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: {
        display: "flex",
        alignItems: "center",
        gap: 6,
        fontSize: compact ? 8 : 9,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        color: "var(--theme-text-muted, #8b8f98)",
        letterSpacing: "0.05em",
        fontWeight: 700
      }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: { flex: 1, height: 1, background: "var(--theme-border, #2a2d35)" } }),
        /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("span", { children: name.toUpperCase() }),
        resolution && /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("span", { style: {
          fontSize: compact ? 7 : 8,
          padding: "1px 4px",
          borderRadius: 2,
          background: "var(--theme-surface, #13151a)",
          border: "1px solid var(--theme-border, #2a2d35)"
        }, children: resolution }),
        /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: { flex: 1, height: 1, background: "var(--theme-border, #2a2d35)" } })
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: {
        fontSize: compact ? 8 : 9,
        color: "var(--theme-text-muted, #6b7080)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        textAlign: "center",
        marginTop: 3
      }, children: [
        "Pool ",
        totalBitrateKbps != null ? `${(totalBitrateKbps / 1e3).toFixed(1)} Mbps` : "\u2014",
        "  \u2022  ",
        "Lag ",
        avgLagMs != null ? `${avgLagMs.toFixed(1)}ms` : "\u2014"
      ] })
    ] });
  }
  function HiddenOutputsToggle({ items, compact = false }) {
    const [expanded, setExpanded] = (0, import_react5.useState)(false);
    if (!items || items.length === 0) return null;
    return /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: { marginTop: 6 }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)(
        "div",
        {
          onClick: () => setExpanded((v) => !v),
          style: {
            display: "flex",
            alignItems: "center",
            gap: 6,
            cursor: "pointer",
            fontSize: compact ? 8 : 9,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            color: "var(--theme-text-muted, #6b7080)"
          },
          children: [
            /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("span", { children: [
              "Hidden (",
              items.length,
              ")"
            ] }),
            /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("span", { style: {
              fontSize: compact ? 7 : 8,
              color: "var(--theme-accent, #5ba3f5)",
              textDecoration: "underline",
              textUnderlineOffset: 2
            }, children: expanded ? "Hide" : "Show" })
          ]
        }
      ),
      expanded && /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: { marginTop: 4, opacity: 0.5 }, children: items.map((item, idx) => /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: {
        fontSize: compact ? 8 : 9,
        color: "var(--theme-text-muted, #6b7080)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        marginBottom: 2
      }, children: [
        item.name || item.platform || `Output ${idx + 1}`,
        item.active ? " (active)" : ""
      ] }, item.id || idx)) })
    ] });
  }
  function OutputConfigRow({ output, config, onUpdate, compact = false }) {
    const [editing, setEditing] = (0, import_react5.useState)(null);
    const inputRef = (0, import_react5.useRef)(null);
    const displayName = config?.displayName || output.name || output.platform || output.id;
    const groupName = config?.group || "";
    const isHidden = config?.hidden || false;
    (0, import_react5.useEffect)(() => {
      if (editing && inputRef.current) inputRef.current.focus();
    }, [editing]);
    const commitEdit = (field, value) => {
      setEditing(null);
      const trimmed = value.trim();
      if (field === "name" && trimmed && trimmed !== displayName) {
        onUpdate({ ...config, displayName: trimmed });
      } else if (field === "group") {
        onUpdate({ ...config, group: trimmed || null });
      }
    };
    const rowFs = compact ? 9 : 10;
    return /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { style: {
      display: "flex",
      alignItems: "center",
      gap: 6,
      padding: "5px 0",
      borderBottom: "1px solid var(--theme-border, #13151a)",
      opacity: isHidden ? 0.4 : 1,
      transition: "opacity 0.2s ease"
    }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime3.jsx)(
        "button",
        {
          onClick: () => onUpdate({ ...config, hidden: !isHidden }),
          title: isHidden ? "Show output" : "Hide output",
          style: {
            width: 16,
            height: 16,
            flexShrink: 0,
            cursor: "pointer",
            background: "none",
            border: "none",
            padding: 0,
            fontSize: 11,
            lineHeight: 1,
            color: isHidden ? "var(--theme-text-muted, #3a3d45)" : "#2ea043"
          },
          children: isHidden ? "\u25CB" : "\u25C9"
        }
      ),
      /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: { flex: 1, minWidth: 0 }, children: editing === "name" ? /* @__PURE__ */ (0, import_jsx_runtime3.jsx)(
        "input",
        {
          ref: inputRef,
          defaultValue: displayName,
          onBlur: (e) => commitEdit("name", e.target.value),
          onKeyDown: (e) => {
            if (e.key === "Enter") commitEdit("name", e.target.value);
            if (e.key === "Escape") setEditing(null);
          },
          style: {
            width: "100%",
            fontSize: rowFs,
            padding: "1px 4px",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            background: "var(--theme-surface, #13151a)",
            color: "var(--theme-text, #e0e2e8)",
            border: "1px solid var(--theme-accent, #5ba3f5)",
            borderRadius: 2,
            outline: "none"
          }
        }
      ) : /* @__PURE__ */ (0, import_jsx_runtime3.jsx)(
        "span",
        {
          onClick: () => setEditing("name"),
          title: "Click to rename",
          style: {
            fontSize: rowFs,
            cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            color: "var(--theme-text, #e0e2e8)",
            fontWeight: 500,
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap",
            display: "block"
          },
          children: displayName
        }
      ) }),
      editing === "group" ? /* @__PURE__ */ (0, import_jsx_runtime3.jsx)(
        "input",
        {
          ref: inputRef,
          defaultValue: groupName,
          placeholder: "Group",
          onBlur: (e) => commitEdit("group", e.target.value),
          onKeyDown: (e) => {
            if (e.key === "Enter") commitEdit("group", e.target.value);
            if (e.key === "Escape") setEditing(null);
          },
          style: {
            width: 60,
            fontSize: compact ? 8 : 9,
            padding: "1px 4px",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            background: "var(--theme-surface, #13151a)",
            color: "var(--theme-text, #e0e2e8)",
            border: "1px solid var(--theme-accent, #5ba3f5)",
            borderRadius: 2,
            outline: "none"
          }
        }
      ) : /* @__PURE__ */ (0, import_jsx_runtime3.jsx)(
        "span",
        {
          onClick: () => setEditing("group"),
          title: "Click to set group",
          style: {
            fontSize: compact ? 8 : 9,
            cursor: "pointer",
            flexShrink: 0,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            color: groupName ? "var(--theme-text-muted, #8b8f98)" : "var(--theme-text-muted, #3a3d45)",
            background: "var(--theme-surface, #13151a)",
            padding: "1px 5px",
            borderRadius: 2,
            border: "1px solid var(--theme-border, #2a2d35)"
          },
          children: groupName || "\u2014"
        }
      )
    ] });
  }
  function OutputConfigPanel({ encoderOutputs, sendAction, compact = false }) {
    const [outputConfig, setOutputConfig] = (0, import_react5.useState)(() => {
      try {
        const raw = localStorage.getItem(OUTPUT_CONFIG_STORAGE_KEY);
        return raw ? JSON.parse(raw) : {};
      } catch (_) {
        return {};
      }
    });
    const allOutputs = (0, import_react5.useMemo)(() => {
      const items = [];
      if (encoderOutputs.groups) {
        for (const group of encoderOutputs.groups) {
          if (group.items) items.push(...group.items);
        }
      }
      if (encoderOutputs.hidden) items.push(...encoderOutputs.hidden);
      return items;
    }, [encoderOutputs]);
    const handleUpdate = (0, import_react5.useCallback)((outputId, newConfig) => {
      setOutputConfig((prev) => {
        const next = { ...prev, [outputId]: newConfig };
        try {
          localStorage.setItem(OUTPUT_CONFIG_STORAGE_KEY, JSON.stringify(next));
        } catch (_) {
        }
        sendAction({ type: "set_output_config", outputId, config: newConfig });
        return next;
      });
    }, [sendAction]);
    if (allOutputs.length === 0) {
      return /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: {
        fontSize: compact ? 9 : 10,
        color: "var(--theme-text-muted, #3a3d45)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        padding: "8px 0",
        textAlign: "center"
      }, children: "No outputs detected" });
    }
    return /* @__PURE__ */ (0, import_jsx_runtime3.jsxs)("div", { children: [
      /* @__PURE__ */ (0, import_jsx_runtime3.jsx)("div", { style: {
        fontSize: compact ? 7 : 8,
        color: "var(--theme-text-muted, #5a5f6d)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        padding: "2px 0 6px",
        letterSpacing: "0.04em"
      }, children: "Click name or group to edit. Toggle visibility with the dot." }),
      allOutputs.map((output) => {
        const id = output.id || output.name || output.platform;
        return /* @__PURE__ */ (0, import_jsx_runtime3.jsx)(
          OutputConfigRow,
          {
            output,
            config: outputConfig[id] || {},
            onUpdate: (cfg) => handleUpdate(id, cfg),
            compact
          },
          id
        );
      })
    ] });
  }

  // connection-components.jsx
  var import_react6 = __require("react");
  var import_jsx_runtime4 = __require("react/jsx-runtime");
  var MANAGED_REGIONS = [
    { id: "us-east", label: "US East" },
    { id: "us-west", label: "US West" },
    { id: "eu-central", label: "EU Central" },
    { id: "ap-southeast", label: "AP Southeast" }
  ];
  function connStatusColor(status) {
    if (status === "connected") return "#2ea043";
    if (status === "connecting") return "#d29922";
    if (status === "error") return "#da3633";
    return "#5a5f6d";
  }
  function freshnessColor(lastMsAgo) {
    if (lastMsAgo == null || lastMsAgo === 0) return "#2ea043";
    if (lastMsAgo < 500) return "#2ea043";
    if (lastMsAgo < 2e3) return "#d29922";
    return "#da3633";
  }
  function matchSignal(carrier, networkConnections) {
    if (!carrier || !Array.isArray(networkConnections) || networkConnections.length === 0) return null;
    const norm = carrier.toLowerCase();
    for (const nc of networkConnections) {
      if (!nc.name) continue;
      const ncNorm = nc.name.toLowerCase();
      if (ncNorm.includes(norm) || norm.includes(ncNorm.replace(/sim\s*\d+\s*[�\-]\s*/i, "").trim())) {
        return { signal: nc.signal, type: nc.type, status: nc.status };
      }
    }
    return null;
  }
  function SignalBarsIcon({ signal, type, color }) {
    const bars = [1, 2, 3, 4];
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("span", { style: { display: "inline-flex", alignItems: "flex-end", gap: 1, marginLeft: 4 }, title: type || "", children: [
      bars.map((i) => /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: {
        display: "inline-block",
        width: 2,
        height: 2 + i * 2,
        borderRadius: 0.5,
        background: i <= signal ? color || "#2ea043" : "var(--theme-border, #2a2d35)"
      } }, i)),
      type && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: {
        fontSize: 7,
        color: "var(--theme-text-muted, #6b7080)",
        marginLeft: 2,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: type })
    ] });
  }
  function ProvisionDots() {
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("span", { children: [
      /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0s" }, children: "." }),
      /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0.2s" }, children: "." }),
      /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0.4s" }, children: "." })
    ] });
  }
  function ManagedProvisionProgress({ step }) {
    const hasStep = step && step.stepNumber > 0;
    const pct = hasStep ? Math.round(step.stepNumber / step.totalSteps * 100) : 15;
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
      marginTop: 4,
      padding: "5px 8px",
      background: "rgba(210,153,34,0.07)",
      borderRadius: 3,
      border: "1px solid #d2992230"
    }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
        display: "flex",
        justifyContent: "space-between",
        fontSize: 9,
        color: "#d29922",
        marginBottom: 3,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("span", { children: [
          hasStep ? step.label : "Starting relay",
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(ProvisionDots, {})
        ] }),
        hasStep && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("span", { children: [
          pct,
          "%"
        ] })
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: { height: 3, borderRadius: 2, background: "var(--theme-border, #2a2d35)", overflow: "hidden" }, children: /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
        height: "100%",
        width: pct + "%",
        borderRadius: 2,
        background: "#d29922",
        transition: "width 0.4s ease"
      } }) })
    ] });
  }
  function ConnectionExpandedDetail({ conn, sendAction, onRemove }) {
    const isByor = conn.type === "byor";
    const [isEditing, setIsEditing] = (0, import_react6.useState)(false);
    const [editName, setEditName] = (0, import_react6.useState)(conn.name || "");
    const [editHost, setEditHost] = (0, import_react6.useState)(conn.relay_host_masked || conn.relay_host || "");
    const [editPort, setEditPort] = (0, import_react6.useState)(String(conn.relay_port || 5e3));
    const [editStreamId, setEditStreamId] = (0, import_react6.useState)(conn.stream_id || "");
    const [editRegion, setEditRegion] = (0, import_react6.useState)(conn.managed_region || "us-east");
    const inputStyle = {
      width: "100%",
      height: 21,
      borderRadius: 3,
      border: "1px solid var(--theme-border, #2a2d35)",
      background: "var(--theme-panel, #20232b)",
      color: "var(--theme-text, #e0e2e8)",
      fontSize: 9,
      padding: "0 6px",
      fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
      boxSizing: "border-box"
    };
    const labelStyle = {
      fontSize: 8,
      color: "var(--theme-text-muted, #8b8f98)",
      textTransform: "uppercase",
      letterSpacing: "0.04em",
      marginBottom: 2,
      fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
    };
    const handleSave = () => {
      const payload = {
        type: "connection_update",
        id: conn.id,
        requestId: genRequestId(),
        name: editName.trim()
      };
      if (isByor) {
        payload.relay_host = editHost.trim();
        payload.relay_port = parseInt(editPort, 10) || 5e3;
        payload.stream_id = editStreamId.trim();
      } else {
        payload.managed_region = editRegion;
      }
      sendAction(payload);
      setIsEditing(false);
    };
    const handleCancelEdit = () => {
      setEditName(conn.name || "");
      setEditHost(conn.relay_host_masked || conn.relay_host || "");
      setEditPort(String(conn.relay_port || 5e3));
      setEditStreamId(conn.stream_id || "");
      setEditRegion(conn.managed_region || "us-east");
      setIsEditing(false);
    };
    if (isEditing) {
      return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
        marginTop: 6,
        padding: "8px 8px",
        background: "var(--theme-surface, #13151a)",
        borderRadius: 4,
        border: "1px solid var(--theme-border, #2a2d35)"
      }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginBottom: 6 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Name" }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
            "input",
            {
              value: editName,
              onChange: (e) => setEditName(e.target.value),
              style: inputStyle
            }
          )
        ] }),
        isByor && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginBottom: 6 }, children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Relay Host" }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
              "input",
              {
                value: editHost,
                onChange: (e) => setEditHost(e.target.value),
                placeholder: "relay.example.com",
                style: inputStyle
              }
            )
          ] }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "grid", gridTemplateColumns: "70px minmax(0, 1fr)", gap: 6, marginBottom: 6 }, children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
              /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Port" }),
              /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
                "input",
                {
                  value: editPort,
                  onChange: (e) => setEditPort(e.target.value.replace(/[^\d]/g, "").slice(0, 5)),
                  inputMode: "numeric",
                  placeholder: "5000",
                  style: inputStyle
                }
              )
            ] }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
              /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Stream ID" }),
              /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
                "input",
                {
                  value: editStreamId,
                  onChange: (e) => setEditStreamId(e.target.value),
                  placeholder: "live/stream",
                  style: inputStyle
                }
              )
            ] })
          ] })
        ] }),
        !isByor && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginBottom: 6 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Region" }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
            "select",
            {
              value: editRegion,
              onChange: (e) => setEditRegion(e.target.value),
              style: { ...inputStyle, height: 23 },
              children: MANAGED_REGIONS.map((r) => /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("option", { value: r.id, children: r.label }, r.id))
            }
          )
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", gap: 6 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: handleSave, style: {
            flex: 1,
            height: 22,
            borderRadius: 3,
            border: "none",
            background: "#2d7aed",
            color: "#fff",
            fontSize: 9,
            fontWeight: 600,
            cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: "Save" }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: handleCancelEdit, style: {
            height: 22,
            padding: "0 10px",
            borderRadius: 3,
            border: "1px solid var(--theme-border, #2a2d35)",
            background: "var(--theme-panel, #20232b)",
            color: "var(--theme-text-muted, #8b8f98)",
            fontSize: 9,
            cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: "Cancel" })
        ] })
      ] });
    }
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginTop: 6 }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
        padding: "8px 8px",
        background: "var(--theme-surface, #13151a)",
        borderRadius: 4,
        border: "1px solid var(--theme-border, #2a2d35)",
        marginBottom: 4
      }, children: [
        isByor && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(SecretField, { label: "Host", value: conn.relay_host_masked || conn.relay_host || "", copyValue: conn.relay_host || conn.relay_host_masked || "" }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 5 }, children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: labelStyle, children: "Port" }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }, children: conn.relay_port || 5e3 })
          ] }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(SecretField, { label: "Stream ID", value: conn.stream_id || "", copyValue: conn.stream_id || "" })
        ] }),
        !isByor && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", justifyContent: "space-between", marginBottom: 5, fontSize: 9 }, children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }, children: "Region" }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { color: "var(--theme-text, #e0e2e8)", fontWeight: 600, fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }, children: conn.managed_region || "\u2014" })
          ] }),
          conn.session_id && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(SecretField, { label: "Session ID", value: conn.session_id, copyValue: conn.session_id }),
          conn.relay_ip && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", justifyContent: "space-between", marginBottom: 5, fontSize: 9 }, children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }, children: "Relay IP" }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { color: "var(--theme-text, #e0e2e8)", fontWeight: 600, fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }, children: conn.relay_ip })
          ] })
        ] }),
        conn.status === "error" && conn.error_msg && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
          fontSize: 9,
          color: "#da3633",
          marginTop: 4,
          padding: "4px 6px",
          background: "rgba(218,54,51,0.08)",
          borderRadius: 3,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: conn.error_msg })
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", gap: 5 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: () => setIsEditing(true), style: {
          flex: 1,
          height: 22,
          borderRadius: 3,
          border: "1px solid var(--theme-border, #2a2d35)",
          background: "var(--theme-panel, #20232b)",
          color: "var(--theme-text-muted, #8b8f98)",
          fontSize: 9,
          cursor: "pointer",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Edit" }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: onRemove, style: {
          height: 22,
          padding: "0 10px",
          borderRadius: 3,
          border: "1px solid #da363340",
          background: "rgba(218,54,51,0.06)",
          color: "#da3633",
          fontSize: 9,
          cursor: "pointer",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Remove" })
      ] })
    ] });
  }
  function getRelayInlineStat(conn) {
    if (conn.status !== "connected") return null;
    const hasPerLink = conn.per_link?.available && Array.isArray(conn.per_link?.links) && conn.per_link.links.length > 0;
    let totalKbps = 0;
    if (hasPerLink) {
      totalKbps = conn.per_link.links.reduce((sum, l) => sum + (l.bitrate_kbps || 0), 0);
    } else if (conn.stats?.available && conn.stats.bitrate_kbps > 0) {
      totalKbps = conn.stats.bitrate_kbps;
    }
    const rttMs = conn.stats?.rtt_ms > 0 ? Math.round(conn.stats.rtt_ms) : 0;
    if (totalKbps === 0 && rttMs === 0) return null;
    const mbps = (totalKbps / 1e3).toFixed(1);
    return { mbps, rttMs };
  }
  function MiniHealthBar({ conn, onClick, isExpanded }) {
    if (conn.status !== "connected") return null;
    const hasPerLink = conn.per_link?.available && Array.isArray(conn.per_link?.links) && conn.per_link.links.length > 0;
    let segments = [];
    if (hasPerLink) {
      const links = conn.per_link.links;
      segments = links.map((l) => {
        const pct = l.sharePct > 0 ? l.sharePct : 100 / links.length;
        const kbps = l.bitrate_kbps || 0;
        const color = kbps === 0 ? "#5a5f6d" : freshnessColor(l.lastMsAgo);
        return { pct, color };
      });
    } else if (conn.stats?.available) {
      const kbps = conn.stats.bitrate_kbps || 0;
      const color = kbps === 0 ? "#5a5f6d" : "#2ea043";
      segments = [{ pct: 100, color }];
    } else {
      return null;
    }
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
      "div",
      {
        onClick,
        title: isExpanded ? "Collapse per-link stats" : "Expand per-link stats",
        style: {
          display: "flex",
          width: 48,
          height: 5,
          borderRadius: 2,
          overflow: "hidden",
          flexShrink: 0,
          cursor: "pointer",
          opacity: isExpanded ? 0.65 : 1,
          transition: "opacity 0.15s"
        },
        children: segments.map((seg, i) => /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: { flex: seg.pct, height: "100%", background: seg.color, minWidth: 2 } }, i))
      }
    );
  }
  function RelayLinkBars({ conn, networkConnections }) {
    if (conn.status !== "connected") return null;
    const hasPerLink = conn.per_link?.available && Array.isArray(conn.per_link?.links) && conn.per_link.links.length > 0;
    const boundedStyle = {
      marginTop: 5,
      paddingLeft: 11,
      maxHeight: 120,
      overflowY: "auto",
      padding: "4px 6px",
      border: "1px solid #2a2d3550",
      borderRadius: 3,
      background: "rgba(255,255,255,0.015)",
      transition: "max-height 0.25s ease"
    };
    if (hasPerLink) {
      const maxKbps = Math.max(...conn.per_link.links.map((l) => l.bitrate_kbps || 0), 1e3) * 1.25;
      return /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: boundedStyle, children: conn.per_link.links.map((link, i) => {
        const kbps = link.bitrate_kbps || 0;
        const color = kbps === 0 ? "#5a5f6d" : freshnessColor(link.lastMsAgo);
        const sig = matchSignal(link.carrier, networkConnections);
        return /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { children: /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
          BitrateBar,
          {
            label: link.carrier || "Link " + (i + 1),
            value: kbps,
            max: maxKbps,
            color,
            suffix: sig ? /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(SignalBarsIcon, { signal: sig.signal, type: sig.type, color }) : null
          }
        ) }, link.carrier || i);
      }) });
    }
    if (conn.stats?.available && conn.stats.bitrate_kbps > 0) {
      const kbps = conn.stats.bitrate_kbps;
      const maxKbps = Math.max(kbps * 1.5, 1e3);
      return /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: boundedStyle, children: /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
        BitrateBar,
        {
          label: "Total",
          value: kbps,
          max: maxKbps,
          color: "#2ea043"
        }
      ) });
    }
    return null;
  }
  function ConnectionRow({ conn, sendAction, isCompact, networkConnections }) {
    const [showDetails, setShowDetails] = (0, import_react6.useState)(false);
    const [showLinks, setShowLinks] = (0, import_react6.useState)(false);
    const statusColor = connStatusColor(conn.status);
    const isConnected = conn.status === "connected";
    const isConnecting = conn.status === "connecting";
    const handleAction = () => {
      if (isConnected || isConnecting) {
        sendAction({ type: "connection_disconnect", id: conn.id, requestId: genRequestId() });
      } else {
        sendAction({ type: "connection_connect", id: conn.id, requestId: genRequestId() });
      }
    };
    const handleRemove = () => {
      sendAction({ type: "connection_remove", id: conn.id, requestId: genRequestId() });
    };
    const actionLabel = isConnected ? "Stop" : isConnecting ? "Cancel" : "Connect";
    const actionColor = isConnected ? "var(--theme-text-muted, #5a5f6d)" : isConnecting ? "#d29922" : "#5ba3f5";
    const showStatus = isConnected || isConnecting || conn.status === "error";
    const statusText = isConnected ? "Active" : isConnecting ? "Connecting\u2026" : conn.error_msg || "Error";
    const statusTextColor = isConnected ? "#2ea043" : isConnecting ? "#d29922" : "#da3633";
    const inlineStat = getRelayInlineStat(conn);
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { borderBottom: "1px solid var(--theme-border, #13151a)", padding: "7px 0" }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", alignItems: "center", gap: 5 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(StatusDot, { color: statusColor, pulse: isConnected || isConnecting }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { flex: 1, overflow: "hidden", minWidth: 0, display: "flex", alignItems: "baseline", gap: 5 }, children: [
          showStatus && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: {
            fontSize: 8,
            fontWeight: 500,
            flexShrink: 0,
            color: statusTextColor,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: statusText }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: {
            fontSize: isCompact ? 9 : 10,
            fontWeight: 600,
            color: "var(--theme-text, #e0e2e8)",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap"
          }, children: conn.name }),
          inlineStat && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("span", { style: {
            fontSize: 8,
            color: "var(--theme-text-muted, #8b8f98)",
            flexShrink: 0,
            whiteSpace: "nowrap",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: [
            inlineStat.mbps,
            " Mbps",
            inlineStat.rttMs > 0 ? " \xB7 " + inlineStat.rttMs + "ms" : ""
          ] })
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(MiniHealthBar, { conn, onClick: () => setShowLinks((s) => !s), isExpanded: showLinks }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(ConnectionTypeBadge, { type: conn.type }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: handleAction, style: {
          height: 20,
          padding: "0 8px",
          borderRadius: 3,
          border: "1px solid var(--theme-border, #2a2d35)",
          background: "var(--theme-panel, #20232b)",
          color: actionColor,
          fontSize: 9,
          fontWeight: 600,
          cursor: "pointer",
          flexShrink: 0,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: actionLabel }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
          "button",
          {
            onClick: () => setShowDetails(!showDetails),
            style: {
              border: "none",
              background: "none",
              color: "var(--theme-text-muted, #4a4f5c)",
              fontSize: 9,
              cursor: "pointer",
              padding: "0 2px",
              flexShrink: 0,
              display: "flex",
              alignItems: "center"
            },
            children: /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { fontSize: 8, lineHeight: 1 }, children: showDetails ? "\u25B2" : "\u25BC" })
          }
        )
      ] }),
      isConnected && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
        maxHeight: showLinks ? 200 : 0,
        overflow: "hidden",
        transition: "max-height 0.2s ease"
      }, children: /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(RelayLinkBars, { conn, networkConnections }) }),
      isConnecting && conn.type === "managed" && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(ManagedProvisionProgress, { step: conn.provision_step }),
      showDetails && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(ConnectionExpandedDetail, { conn, sendAction, onRemove: handleRemove })
    ] });
  }
  function AddConnectionForm({ onClose, sendAction, authAuthenticated, authPlanLabel, authPending, authLogin, authEntitlement, handleAuthLogin, handleAuthOpenBrowser }) {
    const [name, setName] = (0, import_react6.useState)("");
    const [type, setType] = (0, import_react6.useState)("byor");
    const [host, setHost] = (0, import_react6.useState)("");
    const [port, setPort] = (0, import_react6.useState)("5000");
    const [streamId, setStreamId] = (0, import_react6.useState)("");
    const [region, setRegion] = (0, import_react6.useState)("us-east");
    const [error, setError] = (0, import_react6.useState)(null);
    const maxConns = authEntitlement?.max_concurrent_conns || 0;
    const activeConns = authEntitlement?.active_managed_conns || 0;
    const isManagedLimitReached = type === "managed" && authAuthenticated && maxConns > 0 && activeConns >= maxConns;
    const handleAdd = () => {
      const trimName = name.trim();
      if (!trimName) {
        setError("Name is required");
        return;
      }
      if (type === "byor") {
        const trimHost = host.trim();
        if (!trimHost) {
          setError("Relay host is required");
          return;
        }
        const parsedPort = parseInt(port, 10);
        if (!Number.isFinite(parsedPort) || parsedPort < 1 || parsedPort > 65535) {
          setError("Invalid port (1\u201365535)");
          return;
        }
        sendAction({
          type: "connection_add",
          requestId: genRequestId(),
          name: trimName,
          conn_type: "byor",
          relay_host: trimHost,
          relay_port: parsedPort,
          stream_id: streamId.trim()
        });
      } else {
        if (!authAuthenticated) {
          setError("Login required for managed relays");
          return;
        }
        if (isManagedLimitReached) {
          setError("Connection limit reached for your plan");
          return;
        }
        sendAction({
          type: "connection_add",
          requestId: genRequestId(),
          name: trimName,
          conn_type: "managed",
          managed_region: region
        });
      }
      onClose();
    };
    const inputStyle = {
      width: "100%",
      height: 23,
      borderRadius: 3,
      border: "1px solid var(--theme-border, #2a2d35)",
      background: "var(--theme-bg, #0c0e13)",
      color: "var(--theme-text, #e0e2e8)",
      fontSize: 10,
      padding: "0 8px",
      fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
      boxSizing: "border-box"
    };
    const labelStyle = {
      fontSize: 9,
      color: "var(--theme-text-muted, #8b8f98)",
      textTransform: "uppercase",
      letterSpacing: "0.04em",
      marginBottom: 3,
      fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
    };
    const addDisabled = type === "managed" && (!authAuthenticated || isManagedLimitReached);
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
      marginTop: 6,
      background: "var(--theme-surface, #13151a)",
      border: "1px solid var(--theme-border, #2a2d35)",
      borderRadius: 4,
      padding: "10px 10px 12px"
    }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", alignItems: "center", marginBottom: 10 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: {
          flex: 1,
          fontSize: 10,
          fontWeight: 700,
          color: "var(--theme-text, #e0e2e8)",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "New Connection" }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: onClose, style: {
          border: "none",
          background: "none",
          color: "var(--theme-text-muted, #8b8f98)",
          fontSize: 13,
          cursor: "pointer",
          padding: "0 2px",
          lineHeight: 1
        }, children: "\u2715" })
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginBottom: 8 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Name" }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
          "input",
          {
            value: name,
            onChange: (e) => setName(e.target.value),
            placeholder: "e.g. Main Cam \\u2192 My VPS",
            style: inputStyle,
            autoFocus: true
          }
        )
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginBottom: 8 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Type" }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", gap: 5 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: () => setType("byor"), style: {
            flex: 1,
            height: 26,
            borderRadius: 3,
            border: type === "byor" ? "1px solid #2d7aed80" : "1px solid var(--theme-border, #2a2d35)",
            background: type === "byor" ? "#1a3a5a" : "var(--theme-panel, #20232b)",
            color: type === "byor" ? "#5ba3f5" : "var(--theme-text-muted, #8b8f98)",
            fontSize: 9,
            fontWeight: 700,
            cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: "BYOR" }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: () => setType("managed"), style: {
            flex: 1,
            height: 26,
            borderRadius: 3,
            border: type === "managed" ? "1px solid #2ea04380" : "1px solid var(--theme-border, #2a2d35)",
            background: type === "managed" ? "#1a3a2a" : "var(--theme-panel, #20232b)",
            color: type === "managed" ? "#4ade80" : "var(--theme-text-muted, #8b8f98)",
            fontSize: 9,
            fontWeight: 700,
            cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: "Managed" })
        ] })
      ] }),
      type === "byor" && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginBottom: 7 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Relay Host" }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
            "input",
            {
              value: host,
              onChange: (e) => setHost(e.target.value),
              placeholder: "relay.example.com",
              style: inputStyle
            }
          )
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "grid", gridTemplateColumns: "80px minmax(0,1fr)", gap: 7, marginBottom: 7 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Port" }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
              "input",
              {
                value: port,
                onChange: (e) => setPort(e.target.value.replace(/[^\d]/g, "").slice(0, 5)),
                inputMode: "numeric",
                placeholder: "5000",
                style: inputStyle
              }
            )
          ] }),
          /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: labelStyle, children: [
              "Stream ID ",
              /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { textTransform: "none", letterSpacing: 0, opacity: 0.55 }, children: "(optional)" })
            ] }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
              "input",
              {
                value: streamId,
                onChange: (e) => setStreamId(e.target.value),
                placeholder: "live/stream",
                style: inputStyle
              }
            )
          ] })
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
          fontSize: 9,
          color: "var(--theme-text-muted, #4a4f5c)",
          marginBottom: 8,
          padding: "4px 7px",
          background: "var(--theme-panel, #20232b)",
          borderRadius: 3,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Sensitive fields are stored encrypted locally (DPAPI)" })
      ] }),
      type === "managed" && !authAuthenticated && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
        marginBottom: 8,
        padding: "8px 8px",
        background: "var(--theme-panel, #20232b)",
        borderRadius: 3,
        border: "1px solid var(--theme-border, #2a2d35)"
      }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
          fontSize: 10,
          color: "var(--theme-text, #e0e2e8)",
          fontWeight: 600,
          marginBottom: 4,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Login required" }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
          fontSize: 9,
          color: "var(--theme-text-muted, #8b8f98)",
          marginBottom: 7,
          lineHeight: 1.5,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Sign in to provision a Telemy Managed Relay." }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
          "button",
          {
            onClick: handleAuthLogin,
            disabled: authPending,
            style: {
              width: "100%",
              padding: "6px 0",
              border: "1px solid var(--theme-border, #2a2d35)",
              borderRadius: 3,
              background: "var(--theme-surface, #13151a)",
              cursor: authPending ? "not-allowed" : "pointer",
              color: "#5ba3f5",
              fontSize: 10,
              fontWeight: 600,
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              opacity: authPending ? 0.7 : 1
            },
            children: authPending ? "Waiting for browser\u2026" : "Sign In"
          }
        ),
        authPending && authLogin && authLogin.authorize_url && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
          "button",
          {
            onClick: handleAuthOpenBrowser,
            style: {
              width: "100%",
              marginTop: 5,
              padding: "4px 0",
              border: "1px solid var(--theme-border, #2a2d35)",
              borderRadius: 3,
              background: "var(--theme-panel, #20232b)",
              cursor: "pointer",
              color: "var(--theme-text-muted, #8b8f98)",
              fontSize: 9,
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
            },
            children: "Open Browser"
          }
        )
      ] }),
      type === "managed" && authAuthenticated && isManagedLimitReached && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
        marginBottom: 8,
        padding: "6px 8px",
        background: "rgba(210,153,34,0.08)",
        borderRadius: 3,
        border: "1px solid #d2992240"
      }, children: /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
        fontSize: 9,
        color: "#d29922",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: [
        "Connection limit reached for ",
        authPlanLabel,
        " plan (",
        maxConns,
        " max). Upgrade to add more."
      ] }) }),
      type === "managed" && authAuthenticated && !isManagedLimitReached && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { marginBottom: 8 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: labelStyle, children: "Region" }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
          "select",
          {
            value: region,
            onChange: (e) => setRegion(e.target.value),
            style: { ...inputStyle, height: 25, cursor: "pointer" },
            children: MANAGED_REGIONS.map((r) => /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("option", { value: r.id, children: r.label }, r.id))
          }
        )
      ] }),
      error && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
        fontSize: 9,
        color: "#da3633",
        marginBottom: 7,
        padding: "4px 7px",
        background: "rgba(218,54,51,0.08)",
        borderRadius: 3,
        border: "1px solid #da363340",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: error }),
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: { display: "flex", gap: 5 }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: onClose, style: {
          height: 26,
          padding: "0 12px",
          borderRadius: 3,
          border: "1px solid var(--theme-border, #2a2d35)",
          background: "var(--theme-panel, #20232b)",
          color: "var(--theme-text-muted, #8b8f98)",
          fontSize: 10,
          cursor: "pointer",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Cancel" }),
        /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: handleAdd, disabled: addDisabled, style: {
          flex: 1,
          height: 26,
          borderRadius: 3,
          border: "none",
          background: addDisabled ? "var(--theme-panel, #20232b)" : "#2d7aed",
          color: addDisabled ? "var(--theme-text-muted, #5a5f6d)" : "#fff",
          fontSize: 10,
          fontWeight: 600,
          cursor: addDisabled ? "not-allowed" : "pointer",
          opacity: addDisabled ? 0.5 : 1,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Add Relay" })
      ] })
    ] });
  }
  function ConnectionListSection({
    relayConnections,
    networkConnections,
    sendAction,
    authAuthenticated,
    authDisplayName,
    authPlanLabel,
    authPending,
    authLogin,
    authEntitlement,
    authErrorMessage,
    handleAuthLogin,
    handleAuthLogout,
    handleAuthOpenBrowser,
    isCompact
  }) {
    const [showAddModal, setShowAddModal] = (0, import_react6.useState)(false);
    const connections = Array.isArray(relayConnections) ? relayConnections : [];
    return /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { children: [
      /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("div", { style: {
        display: "flex",
        alignItems: "center",
        justifyContent: "space-between",
        marginBottom: 6,
        padding: "5px 8px",
        background: "var(--theme-surface, #13151a)",
        borderRadius: 4,
        border: "1px solid var(--theme-border, #2a2d35)"
      }, children: [
        authAuthenticated ? /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("span", { style: {
          fontSize: 9,
          color: "var(--theme-text, #e0e2e8)",
          flex: 1,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          overflow: "hidden",
          textOverflow: "ellipsis",
          whiteSpace: "nowrap",
          marginRight: 6
        }, children: [
          authDisplayName,
          /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)("span", { style: { color: "var(--theme-text-muted, #5a5f6d)", marginLeft: 5 }, children: [
            "\xB7",
            " ",
            authPlanLabel
          ] })
        ] }) : /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: {
          fontSize: 9,
          color: "var(--theme-text-muted, #5a5f6d)",
          flex: 1,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Sign in for Telemy Relays" }),
        authAuthenticated ? /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("button", { onClick: handleAuthLogout, style: {
          border: "none",
          background: "none",
          color: "var(--theme-text-muted, #4a4f5c)",
          fontSize: 9,
          cursor: "pointer",
          padding: 0,
          flexShrink: 0,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: "Sign out" }) : /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
          "button",
          {
            onClick: handleAuthLogin,
            disabled: authPending,
            style: {
              height: 20,
              padding: "0 8px",
              borderRadius: 3,
              border: "1px solid #2d7aed40",
              background: "#1a3a5a",
              color: "#5ba3f5",
              fontSize: 9,
              fontWeight: 600,
              flexShrink: 0,
              cursor: authPending ? "not-allowed" : "pointer",
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              opacity: authPending ? 0.7 : 1
            },
            children: authPending ? "Waiting\u2026" : "Sign In"
          }
        )
      ] }),
      connections.length === 0 && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("div", { style: {
        textAlign: "center",
        padding: "14px 0",
        color: "var(--theme-text-muted, #4a4f5c)",
        fontSize: 10,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: "No relay connections. Press + to add one." }),
      connections.map((conn) => /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(ConnectionRow, { conn, sendAction, isCompact, networkConnections }, conn.id)),
      !showAddModal && /* @__PURE__ */ (0, import_jsx_runtime4.jsxs)(
        "button",
        {
          onClick: () => setShowAddModal(true),
          style: {
            width: "100%",
            marginTop: connections.length > 0 ? 6 : 2,
            height: 26,
            borderRadius: 4,
            border: "1px dashed var(--theme-border, #2a2d35)",
            background: "transparent",
            color: "var(--theme-text-muted, #4a4f5c)",
            fontSize: 11,
            cursor: "pointer",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            gap: 4,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            transition: "border-color 0.15s, color 0.15s"
          },
          onMouseEnter: (e) => {
            e.currentTarget.style.borderColor = "#2d7aed80";
            e.currentTarget.style.color = "#5ba3f5";
          },
          onMouseLeave: (e) => {
            e.currentTarget.style.borderColor = "var(--theme-border, #2a2d35)";
            e.currentTarget.style.color = "var(--theme-text-muted, #4a4f5c)";
          },
          children: [
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { fontSize: 14, lineHeight: 1, marginTop: -1 }, children: "+" }),
            /* @__PURE__ */ (0, import_jsx_runtime4.jsx)("span", { style: { fontSize: 9, letterSpacing: "0.04em" }, children: "Add Relay" })
          ]
        }
      ),
      showAddModal && /* @__PURE__ */ (0, import_jsx_runtime4.jsx)(
        AddConnectionForm,
        {
          onClose: () => setShowAddModal(false),
          sendAction,
          authAuthenticated,
          authPlanLabel,
          authPending,
          authLogin,
          authEntitlement,
          handleAuthLogin,
          handleAuthOpenBrowser
        }
      )
    ] });
  }

  // aegis-dock.jsx
  var import_jsx_runtime5 = __require("react/jsx-runtime");
  var SECTION_ORDER_STORAGE_KEY = "aegis_section_order_v1";
  var DEFAULT_SECTION_ORDER = [
    "scenes",
    "encoders",
    "bitrate",
    "relay",
    "failover",
    "quickSettings",
    "outputConfig",
    "eventLog"
  ];
  function loadSectionOrder() {
    try {
      const stored = typeof window !== "undefined" && window.localStorage ? window.localStorage.getItem(SECTION_ORDER_STORAGE_KEY) : null;
      if (!stored) return DEFAULT_SECTION_ORDER;
      const parsed = JSON.parse(stored);
      if (!Array.isArray(parsed)) return DEFAULT_SECTION_ORDER;
      const filtered = parsed.filter((id) => DEFAULT_SECTION_ORDER.includes(id));
      const missing = DEFAULT_SECTION_ORDER.filter((id) => !filtered.includes(id));
      return [...filtered, ...missing];
    } catch (_) {
      return DEFAULT_SECTION_ORDER;
    }
  }
  function AegisDock() {
    const dockRootRef = (0, import_react7.useRef)(null);
    const dockLayout = useDockCompactMode(dockRootRef);
    const isCompact = dockLayout === "compact" || dockLayout === "ultra";
    const isUltraCompact = dockLayout === "ultra";
    const bridge = useDockState();
    const sim = useSimulatedState();
    const useBridge = bridge.bridgeAvailable;
    const ds = useBridge ? bridge.state || {} : sim.state;
    const sendAction = useBridge ? bridge.sendAction : sim.sendAction;
    const header = ds.header || {};
    const live = ds.live || {};
    const scenes = ds.scenes || {};
    const conns = ds.connections?.items || [];
    const bitrate = ds.bitrate || {};
    const relay = ds.relay || {};
    const auth = ds.auth || {};
    const failover = ds.failover || {};
    const settings = ds.settings?.items || [];
    const events = ds.events || [];
    const pipe = ds.pipe || {};
    const theme = ds.theme || {};
    const settingsByKey = Object.fromEntries(settings.map((s) => [s.key, s.value]));
    const mode = header.mode || "studio";
    const isLive = live.isLive || false;
    const elapsedSec = live.elapsedSec || 0;
    const authLogin = auth.login || {};
    const authUser = auth.user || {};
    const authEntitlement = auth.entitlement || {};
    const authUsage = auth.usage || {};
    const authAuthenticated = auth.authenticated === true;
    const authHasTokens = auth.hasTokens === true;
    const authPending = authLogin.pending === true;
    const authPollIntervalSeconds = Math.max(2, Number(authLogin.poll_interval_seconds || 3));
    const authReasonCode = authEntitlement.reason_code || auth.lastErrorCode || null;
    const authErrorMessage = auth.lastErrorMessage || authReasonCode || null;
    const authEntitled = authEntitlement.relay_access_status === "enabled";
    const authStateKnown = authAuthenticated || authPending || authHasTokens || !!authUser.id || !!authEntitlement.relay_access_status || !!auth.lastErrorCode;
    const relayLicensed = authStateKnown ? authEntitled : relay.licensed !== false;
    const authDisplayName = authUser.display_name || authUser.email || authUser.id || "Account";
    const authPlanLabel = authEntitlement.plan_tier || "starter";
    const relayConnections = (ds.relay_connections || []).map((conn) => {
      if (conn.status !== "connected") return conn;
      const enriched = { ...conn };
      if (relay.perLinkAvailable && Array.isArray(relay.links) && relay.links.length > 0) {
        const totalKbps = relay.ingestBitrateKbps || 0;
        enriched.per_link = {
          available: true,
          links: relay.links.map((l) => ({
            carrier: l.asn_org || l.addr || "Link",
            bitrate_kbps: Math.round(l.sharePct / 100 * totalKbps),
            lastMsAgo: l.lastMsAgo || 0,
            sharePct: l.sharePct || 0,
            uptimeS: l.uptimeS || 0
          }))
        };
      }
      if (relay.statsAvailable) {
        enriched.stats = {
          available: true,
          bitrate_kbps: relay.ingestBitrateKbps || 0,
          rtt_ms: relay.rttMs || 0
        };
      }
      return enriched;
    });
    const relayActive = relayConnections.some((c) => c.status === "connected") || relay.active === true;
    const derivedMode = relayActive && conns.length > 0 ? "irl" : "studio";
    const authRefreshRequestedRef = (0, import_react7.useRef)(false);
    (0, import_react7.useEffect)(() => {
      if (!useBridge) return;
      if (authRefreshRequestedRef.current) return;
      if (!authHasTokens || authPending) return;
      authRefreshRequestedRef.current = true;
      sendAction({ type: "auth_refresh" });
    }, [useBridge, authHasTokens, authPending, sendAction]);
    (0, import_react7.useEffect)(() => {
      if (!useBridge) return;
      if (!authPending) return;
      const timer = setInterval(() => {
        sendAction({ type: "auth_login_poll" });
      }, authPollIntervalSeconds * 1e3);
      return () => clearInterval(timer);
    }, [useBridge, authPending, authPollIntervalSeconds, sendAction]);
    const engineState = failover.state || (relayActive ? "IRL_ACTIVE" : "STUDIO");
    const healthColor = HEALTH_COLORS[failover.health] || HEALTH_COLORS.offline;
    const healthLabel = (failover.health || "offline").toUpperCase();
    const pipeColor = PIPE_STATUS_COLORS[pipe.status] || PIPE_STATUS_COLORS.down;
    const pipeLabel = pipe.label || (pipe.status === "ok" ? "IPC: OK" : pipe.status === "degraded" ? "IPC: DEGRADED" : "IPC: DOWN");
    const [cachedScenes, setCachedScenes] = (0, import_react7.useState)([]);
    const liveScenes = Array.isArray(scenes.items) && scenes.items.length > 0 ? scenes.items : null;
    const allScenes = liveScenes || cachedScenes;
    const activeScene = allScenes.find((s) => s.id === scenes.activeSceneId) || null;
    const pendingScene = allScenes.find((s) => s.id === scenes.pendingSceneId) || null;
    const [autoSceneRules, setAutoSceneRules] = (0, import_react7.useState)(() => loadAutoSceneRules());
    const [expandedRuleId, setExpandedRuleId] = (0, import_react7.useState)(null);
    const [sceneIntentLinks, setSceneIntentLinks] = (0, import_react7.useState)(() => loadSceneIntentLinks());
    const [sceneIntentLinkNames, setSceneIntentLinkNames] = (0, import_react7.useState)(() => loadSceneIntentLinkNames());
    const [scenePanelExpanded, setScenePanelExpanded] = (0, import_react7.useState)(false);
    const [scenePrefsHydrated, setScenePrefsHydrated] = (0, import_react7.useState)(!useBridge);
    const [sectionOrder, setSectionOrder] = (0, import_react7.useState)(() => loadSectionOrder());
    const [dragSrcId, setDragSrcId] = (0, import_react7.useState)(null);
    const [dragOverId, setDragOverId] = (0, import_react7.useState)(null);
    const scenePrefsSaveTimerRef = (0, import_react7.useRef)(null);
    const scenesItemsRef = (0, import_react7.useRef)(scenes.items);
    scenesItemsRef.current = scenes.items;
    (0, import_react7.useEffect)(() => {
      if (!useBridge) return;
      if (Array.isArray(scenes.items) && scenes.items.length > 0) {
        setCachedScenes(scenes.items);
      }
    }, [useBridge, scenes.items]);
    const resolveSceneIntent = (0, import_react7.useCallback)((scene) => {
      if (!scene) return "OFFLINE";
      const matchedRule = autoSceneRules.find((rule) => sceneIntentLinks[rule.id] === scene.id);
      if (matchedRule) return matchedRule.intent;
      return normalizeIntent(scene.intent) || inferIntentFromName(scene.name);
    }, [sceneIntentLinks, autoSceneRules]);
    const activeIntent = resolveSceneIntent(activeScene);
    const activeSceneRule = autoSceneRules.find((rule) => sceneIntentLinks[rule.id] === scenes.activeSceneId) || autoSceneRules.find((rule) => rule.isDefault) || autoSceneRules[0] || null;
    const activeRuleLinkedScene = activeSceneRule ? allScenes.find((scene) => scene.id === (sceneIntentLinks[activeSceneRule.id] || "")) || null : null;
    const activeRuleSummary = activeSceneRule ? `${activeSceneRule.thresholdEnabled ? `<= ${activeSceneRule.thresholdMbps == null ? "unset" : `${Number(activeSceneRule.thresholdMbps).toFixed(1)} Mbps`}` : "Threshold off"} -> ${activeRuleLinkedScene?.name || "Unlinked"}` : "No auto rule mapped";
    const activeSceneDisplayName = activeSceneRule?.label || activeScene?.name || "No active scene";
    const activeRuleIntentColor = SCENE_INTENT_COLORS[activeSceneRule?.intent] || SCENE_INTENT_COLORS.OFFLINE;
    const activeRuleColor = normalizeOptionalHexColor(activeSceneRule?.bgColor) || activeRuleIntentColor.bg;
    const activeRuleBorderColor = normalizeOptionalHexColor(activeSceneRule?.bgColor) || activeRuleIntentColor.border;
    const autoSceneSwitchEnabled = typeof scenes.autoSwitchEnabled === "boolean" ? scenes.autoSwitchEnabled : typeof settingsByKey.auto_scene_switch === "boolean" ? settingsByKey.auto_scene_switch : null;
    const manualOverrideEnabled = typeof scenes.manualOverrideEnabled === "boolean" ? scenes.manualOverrideEnabled : typeof settingsByKey.manual_override === "boolean" ? settingsByKey.manual_override : null;
    const autoSwitchArmed = typeof scenes.autoSwitchArmed === "boolean" ? scenes.autoSwitchArmed : typeof autoSceneSwitchEnabled === "boolean" ? manualOverrideEnabled === true ? false : autoSceneSwitchEnabled : false;
    const autoSwitchSource = manualOverrideEnabled === true ? "manual_override" : "auto_scene_switch";
    const uiActionGateRef = (0, import_react7.useRef)(/* @__PURE__ */ new Map());
    const tryEnterUiActionGate = (0, import_react7.useCallback)((gateKey, cooldownMs = 400) => {
      const now = Date.now();
      const until = uiActionGateRef.current.get(gateKey) || 0;
      if (until > now) return false;
      uiActionGateRef.current.set(gateKey, now + cooldownMs);
      return true;
    }, []);
    const [autoSwitchToggleLock, setAutoSwitchToggleLock] = (0, import_react7.useState)(null);
    const autoSwitchToggleLockTimerRef = (0, import_react7.useRef)(null);
    const clearAutoSwitchToggleLock = (0, import_react7.useCallback)(() => {
      if (autoSwitchToggleLockTimerRef.current) {
        clearTimeout(autoSwitchToggleLockTimerRef.current);
        autoSwitchToggleLockTimerRef.current = null;
      }
      setAutoSwitchToggleLock(null);
    }, []);
    (0, import_react7.useEffect)(() => {
      if (!autoSwitchToggleLock) return;
      if (autoSwitchArmed === autoSwitchToggleLock.targetArmed) {
        clearAutoSwitchToggleLock();
      }
    }, [autoSwitchArmed, autoSwitchToggleLock, clearAutoSwitchToggleLock]);
    (0, import_react7.useEffect)(() => {
      if (!scenePrefsHydrated) return;
      if (!Array.isArray(scenes.items) || scenes.items.length === 0) return;
      const validSceneIds = new Set(scenes.items.map((s) => s.id));
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
    }, [scenes.items, autoSceneRules, scenePrefsHydrated]);
    (0, import_react7.useEffect)(() => {
      if (!scenePrefsHydrated) return;
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
    }, [scenes.items, autoSceneRules, sceneIntentLinkNames, scenePrefsHydrated]);
    (0, import_react7.useEffect)(() => {
      if (!scenePrefsHydrated) return;
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
    }, [scenes.items, autoSceneRules, scenePrefsHydrated]);
    (0, import_react7.useEffect)(() => {
      if (!scenePrefsHydrated) return;
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
    }, [scenes.items, autoSceneRules, sceneIntentLinks, scenePrefsHydrated]);
    (0, import_react7.useEffect)(() => {
      try {
        if (typeof window === "undefined") return;
        const storage = window.localStorage;
        if (!storage) return;
        storage.setItem(SCENE_LINK_STORAGE_KEY, JSON.stringify(sceneIntentLinks));
      } catch (_) {
      }
    }, [sceneIntentLinks]);
    (0, import_react7.useEffect)(() => {
      try {
        if (typeof window === "undefined") return;
        const storage = window.localStorage;
        if (!storage) return;
        storage.setItem(SCENE_LINK_NAME_STORAGE_KEY, JSON.stringify(sceneIntentLinkNames));
      } catch (_) {
      }
    }, [sceneIntentLinkNames]);
    (0, import_react7.useEffect)(() => {
      try {
        if (typeof window === "undefined") return;
        const storage = window.localStorage;
        if (!storage) return;
        storage.setItem(AUTO_SCENE_RULES_STORAGE_KEY, JSON.stringify(autoSceneRules));
      } catch (_) {
      }
    }, [autoSceneRules]);
    (0, import_react7.useEffect)(() => {
      if (!useBridge) {
        setScenePrefsHydrated(true);
        return;
      }
      setScenePrefsHydrated(false);
      const loadRequestId = `dockprefs_load_${Date.now()}`;
      try {
        sendAction({ type: "load_scene_prefs", requestId: loadRequestId });
      } catch (_) {
      }
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
            const loadedNames = raw.sceneIntentLinksByName && typeof raw.sceneIntentLinksByName === "object" ? normalizeLinkMap(raw.sceneIntentLinksByName) : {};
            if (raw.sceneIntentLinks && typeof raw.sceneIntentLinks === "object") {
              const loadedIds = normalizeLinkMap(raw.sceneIntentLinks);
              const currentScenes = Array.isArray(scenesItemsRef.current) ? scenesItemsRef.current : [];
              if (currentScenes.length > 0) {
                const validIds = new Set(currentScenes.map((s) => s.id));
                const reconciled = { ...loadedIds };
                Object.keys(reconciled).forEach((ruleId) => {
                  if (reconciled[ruleId] && !validIds.has(reconciled[ruleId])) {
                    const savedName = String(loadedNames[ruleId] || "");
                    reconciled[ruleId] = savedName ? findSceneIdByName(savedName, currentScenes) : "";
                  }
                });
                setSceneIntentLinks(reconciled);
              } else {
                setSceneIntentLinks(loadedIds);
              }
            }
            if (Object.keys(loadedNames).length > 0) {
              setSceneIntentLinkNames(loadedNames);
            }
            if (Array.isArray(raw.autoSceneRules)) {
              setAutoSceneRules(normalizeAutoSceneRulesValue(raw.autoSceneRules));
            }
            if (Array.isArray(raw.cachedScenes) && raw.cachedScenes.length > 0) {
              const currentItems = Array.isArray(scenesItemsRef.current) ? scenesItemsRef.current : [];
              if (currentItems.length === 0) {
                setCachedScenes(raw.cachedScenes);
              }
            }
          }
        } catch (_) {
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
    (0, import_react7.useEffect)(() => {
      if (!useBridge) return;
      if (!scenePrefsHydrated) return;
      if (scenePrefsSaveTimerRef.current) {
        clearTimeout(scenePrefsSaveTimerRef.current);
      }
      scenePrefsSaveTimerRef.current = setTimeout(() => {
        const scenesToCache = Array.isArray(scenes.items) && scenes.items.length > 0 ? scenes.items : cachedScenes;
        const payload = {
          sceneIntentLinks,
          sceneIntentLinksByName: sceneIntentLinkNames,
          autoSceneRules,
          cachedScenes: scenesToCache
        };
        try {
          sendAction({
            type: "save_scene_prefs",
            requestId: `dockprefs_save_${Date.now()}`,
            prefsJson: JSON.stringify(payload)
          });
        } catch (_) {
        }
      }, 300);
      return () => {
        if (scenePrefsSaveTimerRef.current) {
          clearTimeout(scenePrefsSaveTimerRef.current);
          scenePrefsSaveTimerRef.current = null;
        }
      };
    }, [useBridge, scenePrefsHydrated, sceneIntentLinks, sceneIntentLinkNames, autoSceneRules, sendAction]);
    (0, import_react7.useEffect)(() => {
      return () => {
        if (autoSwitchToggleLockTimerRef.current) {
          clearTimeout(autoSwitchToggleLockTimerRef.current);
          autoSwitchToggleLockTimerRef.current = null;
        }
      };
    }, []);
    const bondedKbps = bitrate.bondedKbps || 0;
    const relayBondedKbps = relay.statsAvailable ? relay.ingestBitrateKbps : bitrate.relayBondedKbps || bondedKbps;
    const perLinkThroughputs = (0, import_react7.useMemo)(() => {
      if (!relay.perLinkAvailable || !relay.links || relay.links.length === 0) return [];
      return relay.links.map((link, i) => {
        const kbps = relayBondedKbps * (link.sharePct / 100);
        return { ...link, kbps, ...classifyLinkAddr(link.addr, link.asn_org, i) };
      });
    }, [relay.perLinkAvailable, relay.links, relayBondedKbps]);
    const encoderOutputs = ds.outputs || { groups: [], hidden: [] };
    const allEncoderItems = encoderOutputs.groups?.flatMap((g) => g.items) || [];
    const activeOutputCount = allEncoderItems.filter((o) => o.active).length;
    const { maxMap: outputMaxMap, sectionMax: outputSectionMax } = useRollingMaxBitrate(allEncoderItems);
    const link1Bitrate = conns[0]?.bitrate || 0;
    const link2Bitrate = conns[1]?.bitrate || 0;
    const animLink1 = useAnimatedValue(link1Bitrate, 600);
    const animLink2 = useAnimatedValue(link2Bitrate, 600);
    const animBonded = useAnimatedValue(bondedKbps, 600);
    const animRelayBonded = useAnimatedValue(relayBondedKbps, 600);
    const maxPerLink = bitrate.maxPerLinkKbps || 6e3;
    const maxBonded = bitrate.maxBondedKbps || 12e3;
    const autoSwitchBitrateKbps = relayActive ? relayBondedKbps : bondedKbps;
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
            reason: "manual_scene_switch"
          });
        } else {
          sendAction({
            type: "set_setting",
            key: "auto_scene_switch",
            value: false,
            requestId,
            reason: "manual_scene_switch"
          });
        }
      }
      sendAction({ type: "switch_scene", sceneId: scene.id, sceneName: scene.name });
    };
    const handleAuthLogin = (0, import_react7.useCallback)(() => {
      if (!tryEnterUiActionGate("auth_login_start", 1e3)) return;
      sendAction({ type: "auth_login_start", deviceName: "OBS Desktop" });
    }, [sendAction, tryEnterUiActionGate]);
    const handleAuthLogout = (0, import_react7.useCallback)(() => {
      if (!tryEnterUiActionGate("auth_logout", 1e3)) return;
      sendAction({ type: "auth_logout" });
    }, [sendAction, tryEnterUiActionGate]);
    const handleAuthOpenBrowser = (0, import_react7.useCallback)(() => {
      if (!authLogin.authorize_url) return;
      sendAction({ type: "auth_open_browser", authorizeUrl: authLogin.authorize_url });
    }, [sendAction, authLogin.authorize_url]);
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
          requestId
        });
        return;
      }
      sendAction({
        type: "set_setting",
        key: "auto_scene_switch",
        value: targetArmed,
        requestId
      });
    };
    const setSceneIntentLink = (0, import_react7.useCallback)((ruleId, sceneId) => {
      const nextSceneId = String(sceneId || "");
      const scene = allScenes.find((s) => String(s.id || "") === nextSceneId) || null;
      setSceneIntentLinks((prev) => ({ ...prev, [ruleId]: nextSceneId }));
      setSceneIntentLinkNames((prev) => ({ ...prev, [ruleId]: String(scene?.name || "") }));
    }, [allScenes]);
    const updateAutoSceneRule = (0, import_react7.useCallback)((ruleId, patch) => {
      setAutoSceneRules((prev) => prev.map((rule) => rule.id === ruleId ? { ...rule, ...patch } : rule));
    }, []);
    const addAutoSceneRule = (0, import_react7.useCallback)(() => {
      const id = `rule_${Date.now()}_${Math.floor(Math.random() * 1e3)}`;
      setAutoSceneRules((prev) => [
        ...prev,
        { id, label: `Custom ${prev.length + 1}`, intent: "HOLD", thresholdEnabled: true, thresholdMbps: 0.5, isDefault: false, bgColor: "#3a2a1a" }
      ]);
      setExpandedRuleId(id);
    }, []);
    const removeAutoSceneRule = (0, import_react7.useCallback)((ruleId) => {
      setAutoSceneRules((prev) => prev.length <= 1 ? prev : prev.filter((rule) => rule.id !== ruleId));
      setExpandedRuleId((prev) => prev === ruleId ? null : prev);
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
    (0, import_react7.useEffect)(() => {
      if (!relayActive || !autoSwitchArmed) return;
      if ((scenes.items || []).length === 0) return;
      if (scenes.pendingSceneId) return;
      const mbps = autoSwitchBitrateKbps / 1e3;
      const thresholdRules = autoSceneRules.filter((rule) => rule.thresholdEnabled && rule.thresholdMbps != null && Number.isFinite(rule.thresholdMbps)).sort((a, b) => a.thresholdMbps - b.thresholdMbps);
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
        reason: `auto_profile_${targetRule.id}`
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
      tryEnterUiActionGate
    ]);
    const version = header.version || "v0.0.5";
    const activeTheme = { ...OBS_YAMI_GREY_DEFAULTS, ...theme };
    const themeFontFamily = typeof activeTheme.fontFamily === "string" && activeTheme.fontFamily.trim() ? `'${activeTheme.fontFamily.replace(/'/g, "\\'")}', 'Segoe UI', system-ui, sans-serif` : "'Segoe UI', system-ui, sans-serif";
    const isLightTheme = (0, import_react7.useMemo)(() => isLightColor(activeTheme.bg) || isLightColor(activeTheme.surface), [activeTheme.bg, activeTheme.surface]);
    return /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { ref: dockRootRef, style: {
      width: "100%",
      height: "100%",
      background: activeTheme.bg,
      display: "flex",
      flexDirection: "column",
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
      "--theme-font-family": themeFontFamily
    }, children: [
      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("style", { children: getDockCss(activeTheme) }),
      !useBridge && /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
        background: "linear-gradient(90deg, #b33a00 0%, #cc4400 50%, #b33a00 100%)",
        color: "#fff",
        textAlign: "center",
        fontSize: 10,
        fontWeight: 700,
        padding: "4px 0",
        letterSpacing: "0.12em",
        flexShrink: 0,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: "SIMULATION MODE" }),
      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
        height: 2,
        flexShrink: 0,
        background: `linear-gradient(90deg, transparent 0%, ${healthColor} 40%, ${healthColor} 60%, transparent 100%)`,
        boxShadow: `0 0 12px ${healthColor}30, 0 1px 4px ${healthColor}20`,
        animation: "railPulse 3s ease-in-out infinite",
        transition: "background 0.6s ease, box-shadow 0.6s ease"
      } }),
      /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
        padding: isCompact ? "8px 10px 7px" : "10px 12px 8px",
        flexShrink: 0,
        background: `linear-gradient(180deg, var(--theme-panel, #12141a) 0%, var(--theme-bg, #0e1015) 100%)`,
        borderBottom: "1px solid var(--theme-border, #1a1d23)",
        display: "flex",
        alignItems: "center",
        gap: isCompact ? 6 : 8,
        flexWrap: isCompact ? "wrap" : "nowrap"
      }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
          width: isCompact ? 22 : 26,
          height: isCompact ? 22 : 26,
          borderRadius: 5,
          background: `linear-gradient(135deg, ${healthColor}dd 0%, ${healthColor}88 100%)`,
          display: "flex",
          alignItems: "center",
          justifyContent: "center",
          boxShadow: `0 2px 8px ${healthColor}25`,
          transition: "background 0.6s ease, box-shadow 0.6s ease"
        }, children: /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("svg", { width: "14", height: "14", viewBox: "0 0 14 14", fill: "none", children: [
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("path", { d: "M7 1L12.5 4.25V10.75L7 14L1.5 10.75V4.25L7 1Z", stroke: "currentColor", strokeWidth: "1.5", fill: "none" }),
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("circle", { cx: "7", cy: "7.5", r: "2", fill: "currentColor", opacity: "0.9" })
        ] }) }),
        /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { flex: 1, minWidth: 0 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
            fontSize: isCompact ? 11 : 12,
            fontWeight: 700,
            color: "var(--theme-text, #e8eaef)",
            letterSpacing: "0.04em",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: header.title || "Telemy Aegis" }),
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
            fontSize: isCompact ? 7 : 8,
            color: "var(--theme-text-muted, #5a5f6d)",
            fontWeight: 500,
            letterSpacing: "0.06em",
            textTransform: "uppercase",
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap"
          }, children: header.subtitle || "OBS + CORE IPC DOCK" })
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
          display: "flex",
          alignItems: "center",
          gap: 4,
          background: derivedMode === "irl" ? "var(--theme-accent, #1a3a5a)" : "var(--theme-surface, #13151a)",
          borderRadius: 4,
          padding: isCompact ? "4px 8px" : "5px 10px",
          border: `1px solid ${derivedMode === "irl" ? "#2d7aed40" : "var(--theme-border, #1e2028)"}`,
          flexShrink: 0
        }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
            width: 5,
            height: 5,
            borderRadius: "50%",
            background: derivedMode === "irl" ? "#2d7aed" : "var(--theme-text-muted, #5a5f6d)"
          } }),
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
            fontSize: isCompact ? 8 : 9,
            fontWeight: 600,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            textTransform: "uppercase",
            letterSpacing: "0.06em",
            color: derivedMode === "irl" ? "#5ba3f5" : "var(--theme-text-muted, #8b8f98)"
          }, children: derivedMode })
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
          display: "flex",
          alignItems: "center",
          gap: isCompact ? 4 : 5,
          flexShrink: 0,
          background: "var(--theme-surface, #13151a)",
          borderRadius: 4,
          padding: isCompact ? "3px 7px" : "4px 8px",
          border: "1px solid var(--theme-border, #1e2028)"
        }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
            width: 5,
            height: 5,
            borderRadius: "50%",
            background: isLive ? "#2ea043" : "#4a4f5c",
            boxShadow: isLive ? "0 0 6px #2ea04360" : "none",
            animation: isLive ? "pulse 2s ease-in-out infinite" : "none",
            flexShrink: 0
          } }),
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
            fontSize: isCompact ? 8 : 9,
            fontWeight: 600,
            color: isLive ? "#4ade80" : "var(--theme-text-muted, #5a5f6d)",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: formatTime(elapsedSec) }),
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { fontSize: isCompact ? 8 : 9, color: "var(--theme-text-muted, #5a5f6d)" }, children: "\xB7" }),
          /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("span", { style: {
            fontSize: isCompact ? 8 : 9,
            color: "var(--theme-text-muted, #8b8f98)",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
          }, children: [
            (animBonded / 1e3).toFixed(1),
            " Mbps"
          ] })
        ] })
      ] }),
      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { className: "aegis-dock-scroll", style: {
        flex: 1,
        minHeight: 0,
        overflowY: "auto",
        overflowX: "hidden"
      }, children: sectionOrder.map((sectionId) => {
        const isDragOver = dragOverId === sectionId && dragSrcId !== sectionId;
        const isDragging = dragSrcId === sectionId;
        const dragHandleEl = /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
          "div",
          {
            draggable: "true",
            onDragStart: (e) => {
              e.stopPropagation();
              e.dataTransfer.effectAllowed = "move";
              setDragSrcId(sectionId);
            },
            onDragEnd: () => {
              setDragSrcId(null);
              setDragOverId(null);
            },
            title: "Drag to reorder",
            style: {
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              width: 24,
              cursor: "grab",
              flexShrink: 0,
              opacity: isDragging ? 0.8 : 0.3,
              color: "var(--theme-text-muted, #8b8f98)",
              fontSize: 14,
              userSelect: "none",
              transition: "opacity 0.15s"
            },
            children: "\u22EE\u22EE"
          }
        );
        let content = null;
        if (sectionId === "scenes") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(
          Section,
          {
            title: "Scenes",
            icon: "\u25C9",
            defaultOpen: true,
            compact: isCompact,
            badge: activeIntent,
            badgeColor: SCENE_INTENT_COLORS[activeIntent]?.border || "#4a4f5c",
            dragHandle: dragHandleEl,
            children: [
              /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
                marginBottom: 6,
                padding: "6px 7px",
                borderRadius: 4,
                border: `1px solid ${toRgba(activeRuleBorderColor, 0.55)}`,
                background: isLightTheme ? `linear-gradient(135deg, ${toRgba(activeRuleColor, 0.22)} 0%, var(--theme-surface, #13151a) 100%)` : `linear-gradient(135deg, ${toRgba(activeRuleColor, 0.65)} 0%, var(--theme-surface, #13151a) 100%)`
              }, children: /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { display: "flex", alignItems: "center", gap: 6 }, children: [
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                  width: 6,
                  height: 6,
                  borderRadius: "50%",
                  background: activeScene ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)",
                  boxShadow: activeScene ? "0 0 6px var(--theme-accent, #5ba3f5)" : "none",
                  flexShrink: 0
                } }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                  fontSize: isCompact ? 9 : 10,
                  color: "var(--theme-text-muted, #8b8f98)",
                  letterSpacing: "0.04em",
                  textTransform: "uppercase",
                  fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                  flexShrink: 0
                }, children: "Active" }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("span", { style: { minWidth: 0, flex: 1, display: "flex", flexDirection: "column", lineHeight: 1.15 }, children: [
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                    fontSize: isCompact ? 10 : 11,
                    color: "var(--theme-text, #e0e2e8)",
                    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                    overflow: "hidden",
                    textOverflow: "ellipsis",
                    whiteSpace: "nowrap"
                  }, children: activeSceneDisplayName }),
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                    fontSize: 8,
                    color: "var(--theme-text-muted, #8b8f98)",
                    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                    overflow: "hidden",
                    textOverflow: "ellipsis",
                    whiteSpace: "nowrap"
                  }, children: activeRuleSummary })
                ] }),
                pendingScene && /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                  fontSize: 8,
                  color: "var(--theme-accent, #5ba3f5)",
                  fontWeight: 700,
                  letterSpacing: "0.05em"
                }, children: "SWITCHING" }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                  "button",
                  {
                    type: "button",
                    onClick: () => setScenePanelExpanded((prev) => !prev),
                    style: {
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
                      flexShrink: 0
                    },
                    title: scenePanelExpanded ? "Collapse advanced scene controls" : "Expand advanced scene controls",
                    children: scenePanelExpanded ? "\u25B4" : "\u25BE"
                  }
                )
              ] }) }),
              scenePanelExpanded && /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(import_jsx_runtime5.Fragment, { children: [
                /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
                  display: "flex",
                  alignItems: "center",
                  gap: 6,
                  marginBottom: 6,
                  padding: "4px 6px",
                  borderRadius: 4,
                  border: "1px solid var(--theme-border, #2a2d35)",
                  background: "var(--theme-surface, #13151a)"
                }, children: [
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                    fontSize: isCompact ? 8 : 9,
                    color: "var(--theme-text-muted, #8b8f98)",
                    letterSpacing: "0.04em",
                    textTransform: "uppercase",
                    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                    flexShrink: 0,
                    flex: 1
                  }, children: "Scene Rules" }),
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                    "button",
                    {
                      type: "button",
                      onClick: addAutoSceneRule,
                      style: {
                        height: 21,
                        borderRadius: 3,
                        border: "1px solid var(--theme-border, #2a2d35)",
                        background: "var(--theme-panel, #20232b)",
                        color: "var(--theme-text, #e0e2e8)",
                        fontSize: 10,
                        padding: "0 7px",
                        cursor: "pointer",
                        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
                      },
                      children: "+ Rule"
                    }
                  )
                ] }),
                autoSceneRules.map((row) => {
                  const linkedSceneId = sceneIntentLinks[row.id] || "";
                  const linkedScene = (allScenes || []).find((scene) => scene.id === linkedSceneId) || null;
                  const isActive = linkedScene && linkedScene.id === scenes.activeSceneId;
                  const isPending = linkedScene && linkedScene.id === scenes.pendingSceneId;
                  const intentColor = SCENE_INTENT_COLORS[row.intent] || SCENE_INTENT_COLORS.OFFLINE;
                  const isExpanded = expandedRuleId === row.id;
                  const ruleBgColor = normalizeOptionalHexColor(row.bgColor) || intentColor.bg;
                  const ruleBorderColor = normalizeOptionalHexColor(row.bgColor) || intentColor.border;
                  const activeRowBg = isLightTheme ? toRgba(ruleBgColor, 0.14) : ruleBgColor;
                  const activeRowText = isLightTheme || isLightColor(ruleBgColor) ? "var(--theme-text, #1b1d22)" : "var(--theme-text, #e0e2e8)";
                  return /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
                    display: "flex",
                    flexDirection: "column",
                    gap: 5,
                    marginBottom: 5,
                    padding: "5px 6px",
                    borderRadius: 4,
                    border: `1px solid ${isActive ? ruleBorderColor : "var(--theme-border, #2a2d35)"}`,
                    background: isActive ? activeRowBg : "var(--theme-surface, #13151a)"
                  }, children: [
                    /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { display: "grid", gridTemplateColumns: "minmax(0, 1fr) auto", gap: 4, alignItems: "center" }, children: [
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(
                        "button",
                        {
                          type: "button",
                          onClick: () => linkedScene && handleSceneSwitch(linkedScene),
                          disabled: !linkedScene,
                          style: {
                            display: "flex",
                            alignItems: "flex-start",
                            gap: 6,
                            border: "none",
                            background: "transparent",
                            color: isActive ? activeRowText : "var(--theme-text, #e0e2e8)",
                            cursor: linkedScene ? "pointer" : "default",
                            textAlign: "left",
                            padding: 0,
                            minWidth: 0
                          },
                          children: [
                            /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                              width: 5,
                              height: 5,
                              borderRadius: "50%",
                              background: isActive ? ruleBorderColor : isPending ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)",
                              boxShadow: isActive ? `0 0 4px ${ruleBorderColor}` : "none",
                              flexShrink: 0
                            } }),
                            /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("span", { style: {
                              display: "flex",
                              flexDirection: "column",
                              minWidth: 0,
                              lineHeight: 1.15
                            }, children: [
                              /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                                fontSize: isCompact ? 9 : 10,
                                fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                                overflow: "hidden",
                                textOverflow: "ellipsis",
                                whiteSpace: "nowrap",
                                color: isActive ? activeRowText : "var(--theme-text, #e0e2e8)"
                              }, children: row.label }),
                              /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                                fontSize: 8,
                                fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                                overflow: "hidden",
                                textOverflow: "ellipsis",
                                whiteSpace: "nowrap",
                                color: "var(--theme-text-muted, #8b8f98)"
                              }, children: `${row.thresholdEnabled ? `<= ${row.thresholdMbps == null ? "unset" : `${Number(row.thresholdMbps).toFixed(1)} Mbps`}` : "Threshold off"} -> ${linkedScene?.name || "Unlinked"}` })
                            ] })
                          ]
                        }
                      ),
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { display: "flex", alignItems: "center", gap: 3 }, children: [
                        isExpanded && /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(import_jsx_runtime5.Fragment, { children: [
                          RULE_BG_PRESETS.map((preset) => {
                            const selected = normalizeOptionalHexColor(row.bgColor) === normalizeOptionalHexColor(preset.color);
                            return /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                              "button",
                              {
                                type: "button",
                                onClick: () => updateAutoSceneRule(row.id, { bgColor: preset.color }),
                                title: preset.label,
                                style: {
                                  width: 14,
                                  height: 14,
                                  borderRadius: 3,
                                  border: selected ? "1px solid var(--theme-accent, #5ba3f5)" : "1px solid var(--theme-border, #2a2d35)",
                                  background: preset.color,
                                  boxShadow: selected ? `0 0 0 1px ${toRgba(preset.color, 0.55)}` : "none",
                                  padding: 0,
                                  cursor: "pointer"
                                }
                              },
                              `${row.id}_${preset.id}`
                            );
                          }),
                          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                            "label",
                            {
                              title: "Custom color",
                              style: {
                                width: 15,
                                height: 15,
                                borderRadius: "50%",
                                border: "1px solid var(--theme-border, #2a2d35)",
                                overflow: "hidden",
                                cursor: "pointer",
                                display: "inline-block",
                                position: "relative",
                                background: normalizeOptionalHexColor(row.bgColor) || "#2a2d35"
                              },
                              children: /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                                "input",
                                {
                                  type: "color",
                                  value: normalizeOptionalHexColor(row.bgColor) || "#2a2d35",
                                  onChange: (e) => updateAutoSceneRule(row.id, { bgColor: e.target.value }),
                                  style: {
                                    position: "absolute",
                                    inset: 0,
                                    width: "100%",
                                    height: "100%",
                                    border: "none",
                                    padding: 0,
                                    margin: 0,
                                    background: "transparent",
                                    cursor: "pointer",
                                    opacity: 0
                                  }
                                }
                              )
                            }
                          )
                        ] }),
                        /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                          "button",
                          {
                            type: "button",
                            onClick: () => setExpandedRuleId((prev) => prev === row.id ? null : row.id),
                            style: {
                              height: 21,
                              borderRadius: 3,
                              border: "1px solid var(--theme-border, #2a2d35)",
                              background: "var(--theme-panel, #20232b)",
                              color: isExpanded ? "var(--theme-accent, #5ba3f5)" : "var(--theme-text-muted, #8b8f98)",
                              fontSize: 9,
                              padding: "0 8px",
                              cursor: "pointer"
                            },
                            children: isExpanded ? "Close" : "Edit"
                          }
                        )
                      ] })
                    ] }),
                    isExpanded && /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
                      display: "grid",
                      gridTemplateColumns: "minmax(0, 1fr) auto auto minmax(0, 1fr) auto",
                      gap: 4,
                      alignItems: "center",
                      paddingTop: 2
                    }, children: [
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                        "input",
                        {
                          value: row.label,
                          onChange: (e) => updateAutoSceneRule(row.id, { label: e.target.value.slice(0, 40) }),
                          style: {
                            height: 21,
                            borderRadius: 3,
                            border: "1px solid var(--theme-border, #2a2d35)",
                            background: "var(--theme-panel, #20232b)",
                            color: "var(--theme-text, #e0e2e8)",
                            fontSize: 9,
                            padding: "0 6px",
                            minWidth: 80,
                            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
                          }
                        }
                      ),
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                        "input",
                        {
                          type: "number",
                          step: "0.1",
                          min: "0",
                          value: row.thresholdMbps == null ? "" : row.thresholdMbps,
                          onChange: (e) => {
                            const v = e.target.value;
                            updateAutoSceneRule(row.id, {
                              thresholdEnabled: true,
                              thresholdMbps: v === "" ? null : Math.max(0, Number(v) || 0)
                            });
                          },
                          title: "Switch when bitrate is at or below this Mbps value",
                          style: {
                            height: 21,
                            borderRadius: 3,
                            border: "1px solid var(--theme-border, #2a2d35)",
                            background: "var(--theme-panel, #20232b)",
                            color: "var(--theme-text, #e0e2e8)",
                            fontSize: 9,
                            padding: "0 4px",
                            width: 52,
                            textAlign: "right"
                          }
                        }
                      ),
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { fontSize: 9, color: "var(--theme-text-muted, #8b8f98)" }, children: "Mbps" }),
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(
                        "select",
                        {
                          value: linkedSceneId,
                          onChange: (e) => setSceneIntentLink(row.id, e.target.value),
                          style: {
                            height: 21,
                            borderRadius: 3,
                            border: "1px solid var(--theme-border, #2a2d35)",
                            background: "var(--theme-panel, #20232b)",
                            color: "var(--theme-text, #e0e2e8)",
                            fontSize: 9,
                            padding: "0 4px",
                            minWidth: 0
                          },
                          children: [
                            /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("option", { value: "", children: "Select scene..." }),
                            (allScenes || []).map((scene) => /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("option", { value: scene.id, children: scene.name }, scene.id))
                          ]
                        }
                      ),
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                        "button",
                        {
                          type: "button",
                          onClick: () => removeAutoSceneRule(row.id),
                          disabled: autoSceneRules.length <= 1,
                          style: {
                            height: 21,
                            borderRadius: 3,
                            border: "1px solid var(--theme-border, #2a2d35)",
                            background: "var(--theme-panel, #20232b)",
                            color: "var(--theme-text-muted, #8b8f98)",
                            fontSize: 10,
                            padding: "0 6px",
                            cursor: autoSceneRules.length <= 1 ? "not-allowed" : "pointer",
                            opacity: autoSceneRules.length <= 1 ? 0.5 : 1
                          },
                          children: "Remove"
                        }
                      )
                    ] })
                  ] }, row.id);
                })
              ] }),
              /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(
                "button",
                {
                  type: "button",
                  onClick: handleAutoSceneSwitchToggle,
                  disabled: !!autoSwitchToggleLock,
                  style: {
                    marginTop: 8,
                    padding: isCompact ? "5px 7px" : "6px 8px",
                    background: "var(--theme-surface, #1a1d23)",
                    borderRadius: 4,
                    display: "flex",
                    alignItems: "center",
                    gap: 6,
                    border: "1px solid var(--theme-border, #2a2d35)",
                    width: "100%",
                    cursor: autoSwitchToggleLock ? "not-allowed" : "pointer",
                    textAlign: "left",
                    opacity: autoSwitchToggleLock ? 0.75 : 1
                  },
                  children: [
                    /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("svg", { width: "10", height: "10", viewBox: "0 0 10 10", fill: "none", children: [
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                        "path",
                        {
                          d: "M1 5C1 2.8 2.8 1 5 1s4 1.8 4 4-1.8 4-4 4",
                          stroke: autoSwitchArmed ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)",
                          strokeWidth: "1.2",
                          strokeLinecap: "round"
                        }
                      ),
                      /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                        "path",
                        {
                          d: "M3 7L1 5L3 3",
                          stroke: autoSwitchArmed ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)",
                          strokeWidth: "1.2",
                          strokeLinecap: "round",
                          strokeLinejoin: "round"
                        }
                      )
                    ] }),
                    /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { fontSize: 9, color: autoSwitchArmed ? "var(--theme-accent, #5ba3f5)" : "var(--theme-text-muted, #5a5f6d)", fontWeight: 500 }, children: "Auto Scene Switch" }),
                    /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                      fontSize: isCompact ? 8 : 9,
                      fontWeight: 600,
                      marginLeft: "auto",
                      color: autoSwitchArmed ? "#2ea043" : "#da3633"
                    }, children: autoSwitchToggleLock ? isCompact ? "..." : "APPLYING..." : autoSwitchArmed ? "ARMED" : "MANUAL" })
                  ]
                }
              )
            ]
          }
        );
        else if (sectionId === "encoders") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(
          Section,
          {
            title: "Encoders & Uploads",
            icon: "\u229E",
            defaultOpen: true,
            compact: isCompact,
            badge: allEncoderItems.length > 0 ? String(activeOutputCount) : "0",
            badgeColor: activeOutputCount > 0 ? "#2ea043" : "var(--theme-border, #3a3d45)",
            dragHandle: dragHandleEl,
            children: [
              allEncoderItems.length === 0 && /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: { color: "var(--theme-text-muted, #8b8f98)", fontSize: 11, padding: "8px 0", textAlign: "center" }, children: "No encoder outputs detected" }),
              encoderOutputs.groups.map((group, gi) => /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { children: [
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                  EncoderGroupHeader,
                  {
                    name: group.name,
                    resolution: group.resolution,
                    totalBitrateKbps: group.totalBitrateKbps,
                    avgLagMs: group.avgLagMs,
                    compact: isCompact
                  }
                ),
                group.items.map((item, ii) => /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
                  OutputBar,
                  {
                    name: item.name || item.platform,
                    bitrateKbps: item.kbps,
                    fps: item.fps,
                    dropPct: item.dropPct,
                    active: item.active !== false,
                    maxBitrate: outputMaxMap[item.id || item.name || item.platform] || outputSectionMax,
                    compact: isCompact
                  },
                  item.id || `${gi}-${ii}`
                ))
              ] }, group.name || gi)),
              encoderOutputs.hidden?.length > 0 && /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(HiddenOutputsToggle, { items: encoderOutputs.hidden, compact: isCompact })
            ]
          }
        );
        else if (sectionId === "bitrate") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
          Section,
          {
            title: "Bitrate",
            icon: "\u25A5",
            defaultOpen: true,
            compact: isCompact,
            dragHandle: dragHandleEl,
            children: /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
              marginTop: 8,
              display: "grid",
              gridTemplateColumns: isUltraCompact ? "1fr" : "1fr 1fr",
              gap: 4,
              fontSize: 9,
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
            }, children: [
              /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
                background: "var(--theme-surface, #13151a)",
                borderRadius: 3,
                padding: "5px 7px",
                border: "1px solid var(--theme-border, #1e2028)"
              }, children: [
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: { color: "var(--theme-text-muted, #5a5f6d)", marginBottom: 2 }, children: "LOW THRESHOLD" }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: { color: "#fbbf24", fontWeight: 600 }, children: bitrate.lowThresholdMbps != null ? `${bitrate.lowThresholdMbps.toFixed(1)} Mbps` : "\u2014" })
              ] }),
              /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
                background: "var(--theme-surface, #13151a)",
                borderRadius: 3,
                padding: "5px 7px",
                border: "1px solid var(--theme-border, #1e2028)"
              }, children: [
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: { color: "var(--theme-text-muted, #5a5f6d)", marginBottom: 2 }, children: "BRB THRESHOLD" }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: { color: "#da3633", fontWeight: 600 }, children: bitrate.brbThresholdMbps != null ? `${bitrate.brbThresholdMbps.toFixed(1)} Mbps` : "\u2014" })
              ] })
            ] })
          }
        );
        else if (sectionId === "relay") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
          Section,
          {
            title: "Relay",
            icon: "\u2601",
            compact: isCompact,
            defaultOpen: true,
            badge: relayConnections.length > 0 ? String(relayConnections.filter((c) => c.status === "connected").length) + "/" + String(relayConnections.length) : "0",
            badgeColor: relayConnections.some((c) => c.status === "connected") ? "#2d7aed" : "var(--theme-border, #3a3d45)",
            dragHandle: dragHandleEl,
            children: /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
              ConnectionListSection,
              {
                relayConnections,
                networkConnections: conns,
                sendAction,
                authAuthenticated,
                authDisplayName,
                authPlanLabel,
                authPending,
                authLogin,
                authEntitlement,
                authErrorMessage,
                handleAuthLogin,
                handleAuthLogout,
                handleAuthOpenBrowser,
                isCompact
              }
            )
          }
        );
        else if (sectionId === "failover") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)(
          Section,
          {
            title: "Failover Engine",
            icon: "\u26A1",
            compact: isCompact,
            dragHandle: dragHandleEl,
            children: [
              /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
                display: "flex",
                alignItems: "center",
                gap: 4,
                marginBottom: 8,
                padding: "6px 8px",
                background: "var(--theme-surface, #13151a)",
                borderRadius: 4,
                border: `1px solid ${healthColor}40`
              }, children: [
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
                  width: 6,
                  height: 6,
                  borderRadius: "50%",
                  background: healthColor,
                  boxShadow: `0 0 6px ${healthColor}40`,
                  flexShrink: 0
                } }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { fontSize: 10, color: healthColor, fontWeight: 600 }, children: healthLabel }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { fontSize: 9, color: "var(--theme-text-muted, #5a5f6d)", marginLeft: "auto" }, children: engineState })
              ] }),
              /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(EngineStateChips, { activeState: engineState, compact: isUltraCompact }),
              /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { fontSize: 9, color: "var(--theme-text-muted, #5a5f6d)", lineHeight: 1.6 }, children: [
                /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { display: "flex", justifyContent: "space-between", marginBottom: 2 }, children: [
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { children: "Response Budget" }),
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("span", { style: { color: "#2ea043", fontWeight: 600 }, children: [
                    "< ",
                    failover.responseBudgetMs || 800,
                    "ms"
                  ] })
                ] }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { display: "flex", justifyContent: "space-between", marginBottom: 2 }, children: [
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { children: "Last Transition" }),
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { color: "var(--theme-text-muted, #8b8f98)" }, children: failover.lastFailoverLabel || "\u2014" })
                ] }),
                /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { display: "flex", justifyContent: "space-between" }, children: [
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { children: "Total Transitions" }),
                  /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { color: "var(--theme-text-muted, #8b8f98)" }, children: failover.totalFailoversLabel || "\u2014" })
                ] })
              ] })
            ]
          }
        );
        else if (sectionId === "quickSettings") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
          Section,
          {
            title: "Quick Settings",
            icon: "\u2699",
            compact: isCompact,
            dragHandle: dragHandleEl,
            children: settings.filter((setting) => !["manual_override", "auto_scene_switch"].includes(setting.key)).map((setting) => /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
              ToggleRow,
              {
                label: setting.label,
                value: setting.value,
                color: SETTING_COLORS[setting.key] || "#2d7aed",
                dimmed: setting.value === null,
                onChange: (val) => handleSettingChange(setting.key, val)
              },
              setting.key
            ))
          }
        );
        else if (sectionId === "outputConfig") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
          Section,
          {
            title: "Output Config",
            icon: "\u2699",
            compact: isCompact,
            badge: String(allEncoderItems.length + (encoderOutputs.hidden?.length || 0)),
            badgeColor: "var(--theme-border, #3a3d45)",
            dragHandle: dragHandleEl,
            children: allEncoderItems.length === 0 ? /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: { color: "var(--theme-text-muted, #8b8f98)", fontSize: 11, padding: "8px 0", textAlign: "center" }, children: "No outputs configured" }) : /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
              OutputConfigPanel,
              {
                encoderOutputs,
                sendAction,
                compact: isCompact
              }
            )
          }
        );
        else if (sectionId === "eventLog") content = /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
          Section,
          {
            title: "Event Log",
            icon: "\u25A4",
            compact: isCompact,
            badge: String(events.length),
            badgeColor: "var(--theme-surface, #3a3d45)",
            dragHandle: dragHandleEl,
            children: events.map((e, i) => /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
              display: "flex",
              gap: 8,
              padding: "4px 0",
              borderBottom: i < events.length - 1 ? "1px solid var(--theme-border, #13151a)" : "none",
              animation: `slideIn 0.3s ease ${i * 0.05}s both`
            }, children: [
              /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                fontSize: 9,
                color: "var(--theme-text-muted, #3a3d45)",
                fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                flexShrink: 0
              }, children: e.time }),
              /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
                fontSize: 9,
                fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                color: e.type === "success" ? "#4ade80" : e.type === "warning" ? "#fbbf24" : e.type === "error" ? "#da3633" : "var(--theme-text-muted, #6b7080)",
                wordBreak: "break-word"
              }, children: e.msg })
            ] }, e.id || i))
          }
        );
        if (!content) return null;
        return /* @__PURE__ */ (0, import_jsx_runtime5.jsx)(
          "div",
          {
            onDragOver: (e) => {
              e.preventDefault();
              if (dragSrcId && dragSrcId !== sectionId) setDragOverId(sectionId);
            },
            onDrop: (e) => {
              e.preventDefault();
              if (!dragSrcId || dragSrcId === sectionId) {
                setDragSrcId(null);
                setDragOverId(null);
                return;
              }
              setSectionOrder((prev) => {
                const next = [...prev];
                const fromIdx = next.indexOf(dragSrcId);
                const toIdx = next.indexOf(sectionId);
                if (fromIdx < 0 || toIdx < 0) return prev;
                next.splice(fromIdx, 1);
                next.splice(toIdx, 0, dragSrcId);
                try {
                  window.localStorage && window.localStorage.setItem(SECTION_ORDER_STORAGE_KEY, JSON.stringify(next));
                } catch (_) {
                }
                return next;
              });
              setDragSrcId(null);
              setDragOverId(null);
            },
            style: {
              outline: isDragOver ? "1px solid var(--theme-accent, #2d7aed)" : "none",
              opacity: isDragging ? 0.4 : 1,
              transition: "opacity 0.15s"
            },
            children: content
          },
          sectionId
        );
      }) }),
      /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: {
        padding: isCompact ? "6px 10px" : "7px 12px",
        flexShrink: 0,
        borderTop: "1px solid var(--theme-border, #1a1d23)",
        background: "var(--theme-bg, #0c0e13)",
        display: "flex",
        alignItems: "center",
        justifyContent: "space-between",
        gap: 6,
        flexWrap: isUltraCompact ? "wrap" : "nowrap"
      }, children: [
        /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("span", { style: {
          fontSize: 8,
          color: "var(--theme-text-muted, #3a3d45)",
          letterSpacing: "0.06em",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
        }, children: [
          "TELEMY ",
          version.toUpperCase?.() || version
        ] }),
        /* @__PURE__ */ (0, import_jsx_runtime5.jsxs)("div", { style: { display: "flex", alignItems: "center", gap: 4 }, children: [
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: {
            fontSize: 8,
            color: pipeColor,
            textShadow: `0 0 4px ${pipeColor}40`
          }, children: "\u25CF" }),
          /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("span", { style: { fontSize: 8, color: "var(--theme-text-muted, #3a3d45)" }, children: pipeLabel })
        ] })
      ] }),
      !useBridge && /* @__PURE__ */ (0, import_jsx_runtime5.jsx)("div", { style: {
        position: "absolute",
        top: 6,
        right: 6,
        fontSize: 7,
        color: "#da3633",
        background: "#260d0d",
        padding: "1px 5px",
        borderRadius: 2,
        border: "1px solid #3a1a1a",
        letterSpacing: "0.06em",
        fontWeight: 700,
        opacity: 0.8,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)"
      }, children: "SIM" })
    ] });
  }

  // aegis-dock-entry.jsx
  var import_jsx_runtime6 = __require("react/jsx-runtime");
  try {
    const root = document.getElementById("root");
    if (!root) {
      document.body.innerHTML = '<pre style="color:red;padding:10px">ERROR: #root element not found</pre>';
    } else {
      (0, import_client.createRoot)(root).render(/* @__PURE__ */ (0, import_jsx_runtime6.jsx)(AegisDock, {}));
    }
  } catch (err) {
    document.body.innerHTML = '<pre style="color:red;padding:10px;white-space:pre-wrap">MOUNT ERROR: ' + String(err && err.stack || err) + "</pre>";
  }
})();
