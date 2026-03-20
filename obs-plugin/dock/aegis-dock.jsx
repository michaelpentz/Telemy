import { useState, useEffect, useRef, useCallback, useMemo } from "react";
import {
  ENGINE_STATES, HEALTH_COLORS, SCENE_INTENT_COLORS, PIPE_STATUS_COLORS,
  SETTING_COLORS, UI_ACTION_COOLDOWNS_MS, AUTO_SWITCH_LOCK_TIMEOUT_MS,
  OUTPUT_HEALTH_COLORS, getOutputHealthColor,
  SCENE_LINK_STORAGE_KEY, SCENE_LINK_NAME_STORAGE_KEY,
  AUTO_SCENE_RULES_STORAGE_KEY, OUTPUT_CONFIG_STORAGE_KEY,
  DEFAULT_AUTO_SCENE_RULES, RULE_BG_PRESETS, SCENE_PROFILE_NAME_HINTS,
  OBS_YAMI_GREY_DEFAULTS, SIM_SCENES, SIM_SETTING_DEFS, SIM_EVENTS
} from "./constants.js";
import {
  genRequestId, formatTime, parseHexColor, toRgba, isLightColor,
  normalizeOptionalHexColor, getDefaultRuleBgColor, normalizeIntent,
  inferIntentFromName, normalizeSceneName, findBestSceneIdForRule,
  mapRelayStatusForUi, loadSceneIntentLinks, loadSceneIntentLinkNames,
  findSceneIdByName, normalizeLinkMap, loadAutoSceneRules,
  normalizeAutoSceneRulesValue, cefCopyToClipboard, classifyLinkAddr
} from "./utils.js";
import { getDockCss } from "./css.js";
import { useAnimatedValue, useRollingMaxBitrate, useDockCompactMode } from "./hooks.js";
import { useDockState } from "./use-dock-state.js";
import { useSimulatedState } from "./use-simulated-state.js";
import {
  Section, StatusDot, BitrateBar, StatPill, ToggleRow,
  EngineStateChips
} from "./ui-components.jsx";
import { SceneButton } from "./scene-components.jsx";
import {
  OutputBar, EncoderGroupHeader, HiddenOutputsToggle,
  OutputConfigRow, OutputConfigPanel
} from "./encoder-components.jsx";
import { ConnectionListSection } from "./connection-components.jsx";

// =============================================================================
// SECTION ORDER PERSISTENCE
// =============================================================================

const SECTION_ORDER_STORAGE_KEY = "aegis_section_order_v1";
const DEFAULT_SECTION_ORDER = [
  "scenes", "encoders", "bitrate", "relay",
  "failover", "quickSettings", "outputConfig", "eventLog",
];

function loadSectionOrder() {
  try {
    const stored = typeof window !== "undefined" && window.localStorage
      ? window.localStorage.getItem(SECTION_ORDER_STORAGE_KEY)
      : null;
    if (!stored) return DEFAULT_SECTION_ORDER;
    const parsed = JSON.parse(stored);
    if (!Array.isArray(parsed)) return DEFAULT_SECTION_ORDER;
    const filtered = parsed.filter(id => DEFAULT_SECTION_ORDER.includes(id));
    const missing = DEFAULT_SECTION_ORDER.filter(id => !filtered.includes(id));
    return [...filtered, ...missing];
  } catch (_) {
    return DEFAULT_SECTION_ORDER;
  }
}

// =============================================================================
// MAIN DOCK COMPONENT
// =============================================================================

function ProvisionDots() {
  return (
    <span>
      <span style={{ animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0s" }}>.</span>
      <span style={{ animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0.2s" }}>.</span>
      <span style={{ animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0.4s" }}>.</span>
    </span>
  );
}

function RelayProvisionProgress({ step }) {
  const hasStep = step && step.stepNumber > 0;
  const pct = hasStep ? Math.round((step.stepNumber / step.totalSteps) * 100) : 0;
  return (
    <div style={{
      background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "10px 10px",
      border: "1px solid #d2992240",
    }}>
      <div style={{ fontSize: 10, color: "#d29922", fontWeight: 600 }}>
        {hasStep ? step.label : "Provisioning relay"}
        <ProvisionDots />
      </div>
      {hasStep && (
        <div style={{ marginTop: 6 }}>
          <div style={{ display: "flex", justifyContent: "space-between", fontSize: 9, color: "var(--theme-text-muted, #8b8f98)", marginBottom: 3 }}>
            <span>Step {step.stepNumber} / {step.totalSteps}</span>
            <span>{pct}%</span>
          </div>
          <div style={{ height: 3, borderRadius: 2, background: "var(--theme-border, #2a2d35)", overflow: "hidden" }}>
            <div style={{ height: "100%", width: pct + "%", borderRadius: 2, background: "#d29922", transition: "width 0.4s ease" }} />
          </div>
        </div>
      )}
      {!hasStep && (
        <div style={{ fontSize: 9, color: "var(--theme-text-muted, #8b8f98)", marginTop: 4, textAlign: "center" }}>
          This may take a few minutes
        </div>
      )}
    </div>
  );
}

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
  const auth    = ds.auth || {};
  const failover = ds.failover || {};
  const settings = ds.settings?.items || [];
  const events  = ds.events || [];
  const pipe    = ds.pipe || {};
  const theme   = ds.theme || {};
  const settingsByKey = Object.fromEntries(settings.map(s => [s.key, s.value]));

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
  const relayLicensed = authStateKnown ? authEntitled : (relay.licensed !== false);
  const authDisplayName = authUser.display_name || authUser.email || authUser.id || "Account";
  const authPlanLabel = authEntitlement.plan_tier || "starter";

  // Relay connections list (v0.0.5 multi-connection model)
  // Enrich connected connections with global per-link + stats from bridge relay state
  const relayConnections = (ds.relay_connections || []).map(conn => {
    if (conn.status !== "connected") return conn;
    const enriched = { ...conn };
    if (relay.perLinkAvailable && Array.isArray(relay.links) && relay.links.length > 0) {
      const totalKbps = relay.ingestBitrateKbps || 0;
      enriched.per_link = {
        available: true,
        links: relay.links.map(l => ({
          carrier: l.asn_org || l.addr || "Link",
          bitrate_kbps: Math.round((l.sharePct / 100) * totalKbps),
        })),
      };
    }
    if (relay.statsAvailable) {
      enriched.stats = {
        available: true,
        bitrate_kbps: relay.ingestBitrateKbps || 0,
        rtt_ms: relay.rttMs || 0,
      };
    }
    return enriched;
  });
  // relayActive: any relay connection is connected, or legacy bridge relay.active
  const relayActive = relayConnections.some(c => c.status === "connected") || relay.active === true;
  const derivedMode = relayActive && conns.length > 0 ? "irl" : "studio";

  const authRefreshRequestedRef = useRef(false);

  useEffect(() => {
    if (!useBridge) return;
    if (authRefreshRequestedRef.current) return;
    if (!authHasTokens || authPending) return;
    authRefreshRequestedRef.current = true;
    sendAction({ type: "auth_refresh" });
  }, [useBridge, authHasTokens, authPending, sendAction]);

  useEffect(() => {
    if (!useBridge) return;
    if (!authPending) return;
    const timer = setInterval(() => {
      sendAction({ type: "auth_login_poll" });
    }, authPollIntervalSeconds * 1000);
    return () => clearInterval(timer);
  }, [useBridge, authPending, authPollIntervalSeconds, sendAction]);


  // Failover / health
  const engineState = failover.state || (relayActive ? "IRL_ACTIVE" : "STUDIO");
  const healthColor = HEALTH_COLORS[failover.health] || HEALTH_COLORS.offline;
  const healthLabel = (failover.health || "offline").toUpperCase();

  // Pipe status
  const pipeColor = PIPE_STATUS_COLORS[pipe.status] || PIPE_STATUS_COLORS.down;
  const pipeLabel = pipe.label || (pipe.status === "ok" ? "IPC: OK" : pipe.status === "degraded" ? "IPC: DEGRADED" : "IPC: DOWN");

  // Active scene (matched by ID)
  // cachedScenes: restored from prefs file as fallback until the C++ scene
  // snapshot arrives.  Prevents the dropdown from being empty on dock refresh.
  const [cachedScenes, setCachedScenes] = useState([]);
  const liveScenes = Array.isArray(scenes.items) && scenes.items.length > 0 ? scenes.items : null;
  const allScenes = liveScenes || cachedScenes;
  const activeScene = allScenes.find((s) => s.id === scenes.activeSceneId) || null;
  const pendingScene = allScenes.find((s) => s.id === scenes.pendingSceneId) || null;
  const [autoSceneRules, setAutoSceneRules] = useState(() => loadAutoSceneRules());
  const [expandedRuleId, setExpandedRuleId] = useState(null);
  const [sceneIntentLinks, setSceneIntentLinks] = useState(() => loadSceneIntentLinks());
  const [sceneIntentLinkNames, setSceneIntentLinkNames] = useState(() => loadSceneIntentLinkNames());
  // autoSwitchSourceSelection removed — now derived from relayActive
  const [scenePanelExpanded, setScenePanelExpanded] = useState(false);
  const [scenePrefsHydrated, setScenePrefsHydrated] = useState(!useBridge);
  const [sectionOrder, setSectionOrder] = useState(() => loadSectionOrder());
  const [dragSrcId, setDragSrcId] = useState(null);
  const [dragOverId, setDragOverId] = useState(null);
  const scenePrefsSaveTimerRef = useRef(null);
  const scenesItemsRef = useRef(scenes.items);
  scenesItemsRef.current = scenes.items;

  // Sync cachedScenes whenever live scenes arrive from the C++ snapshot.
  // IMPORTANT: only sync when the real bridge is active — otherwise the
  // first render (useBridge=false) briefly uses SIM_SCENES as scenes.items,
  // which would contaminate cachedScenes with rule-label placeholder names.
  useEffect(() => {
    if (!useBridge) return;
    if (Array.isArray(scenes.items) && scenes.items.length > 0) {
      setCachedScenes(scenes.items);
    }
  }, [useBridge, scenes.items]);

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
  const autoSwitchSource = manualOverrideEnabled === true ? "manual_override" : "auto_scene_switch";

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

  // Effect A: prune stale scene IDs (gated on hydration AND scene availability
  // to prevent nuking all links before the C++ snapshot arrives after refresh)
  useEffect(() => {
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

  // Effect B: remap stale IDs via saved scene names
  useEffect(() => {
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

  // Effect C: auto-fill empty slots via name hints
  useEffect(() => {
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

  // Effect D: mirror current scene names for persistence
  useEffect(() => {
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
          const loadedNames = (raw.sceneIntentLinksByName && typeof raw.sceneIntentLinksByName === "object")
            ? normalizeLinkMap(raw.sceneIntentLinksByName) : {};
          if (raw.sceneIntentLinks && typeof raw.sceneIntentLinks === "object") {
            const loadedIds = normalizeLinkMap(raw.sceneIntentLinks);
            // Reconcile stale IDs against current scene list by name,
            // but only if scenes have arrived — otherwise keep loaded IDs as-is
            // and let Effects A/B handle reconciliation when scenes appear.
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
              // Scenes not loaded yet — trust the saved IDs
              setSceneIntentLinks(loadedIds);
            }
          }
          if (Object.keys(loadedNames).length > 0) {
            setSceneIntentLinkNames(loadedNames);
          }
          // autoSwitchSourceSelection no longer loaded — derived from relayActive
          if (Array.isArray(raw.autoSceneRules)) {
            setAutoSceneRules(normalizeAutoSceneRulesValue(raw.autoSceneRules));
          }
          // Restore cached scenes so the dropdown populates immediately
          // on dock refresh, before the C++ scene snapshot arrives.
          if (Array.isArray(raw.cachedScenes) && raw.cachedScenes.length > 0) {
            const currentItems = Array.isArray(scenesItemsRef.current) ? scenesItemsRef.current : [];
            if (currentItems.length === 0) {
              setCachedScenes(raw.cachedScenes);
            }
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
      // Include cachedScenes so the dropdown can populate immediately on
      // dock refresh, before the C++ scene snapshot arrives.
      const scenesToCache = Array.isArray(scenes.items) && scenes.items.length > 0
        ? scenes.items : cachedScenes;
      const payload = {
        sceneIntentLinks,
        sceneIntentLinksByName: sceneIntentLinkNames,
        autoSceneRules,
        cachedScenes: scenesToCache,
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
  const relayBondedKbps = relay.statsAvailable ? relay.ingestBitrateKbps : (bitrate.relayBondedKbps || bondedKbps);
  // Per-link throughput derived from share_pct × bonded bitrate
  const perLinkThroughputs = useMemo(() => {
    if (!relay.perLinkAvailable || !relay.links || relay.links.length === 0) return [];
    return relay.links.map((link, i) => {
      const kbps = relayBondedKbps * (link.sharePct / 100);
      return { ...link, kbps, ...classifyLinkAddr(link.addr, link.asn_org, i) };
    });
  }, [relay.perLinkAvailable, relay.links, relayBondedKbps]);
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

  const handleAuthLogin = useCallback(() => {
    if (!tryEnterUiActionGate("auth_login_start", 1000)) return;
    sendAction({ type: "auth_login_start", deviceName: "OBS Desktop" });
  }, [sendAction, tryEnterUiActionGate]);

  const handleAuthLogout = useCallback(() => {
    if (!tryEnterUiActionGate("auth_logout", 1000)) return;
    sendAction({ type: "auth_logout" });
  }, [sendAction, tryEnterUiActionGate]);

  const handleAuthOpenBrowser = useCallback(() => {
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
  const version = header.version || "v0.0.4";

  // Merge runtime theme with defaults for safer property access
  const activeTheme = { ...OBS_YAMI_GREY_DEFAULTS, ...theme };
  const themeFontFamily = (typeof activeTheme.fontFamily === "string" && activeTheme.fontFamily.trim())
    ? `'${activeTheme.fontFamily.replace(/'/g, "\\'")}', 'Segoe UI', system-ui, sans-serif`
    : "'Segoe UI', system-ui, sans-serif";
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
      {/* SIMULATION MODE BANNER (RF-019)                                   */}
      {/* ================================================================= */}
      {!useBridge && <div style={{
        background: "linear-gradient(90deg, #b33a00 0%, #cc4400 50%, #b33a00 100%)",
        color: "#fff", textAlign: "center", fontSize: 10, fontWeight: 700,
        padding: "4px 0", letterSpacing: "0.12em", flexShrink: 0,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
      }}>SIMULATION MODE</div>}

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
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            textTransform: "uppercase", letterSpacing: "0.06em",
            color: derivedMode === "irl" ? "#5ba3f5" : "var(--theme-text-muted, #8b8f98)",
          }}>
            {derivedMode}
          </span>
        </div>
        {/* Live indicator + time + Mbps — inline in header */}
        <div style={{
          display: "flex", alignItems: "center", gap: isCompact ? 4 : 5, flexShrink: 0,
          background: "var(--theme-surface, #13151a)",
          borderRadius: 4, padding: isCompact ? "3px 7px" : "4px 8px",
          border: "1px solid var(--theme-border, #1e2028)",
        }}>
          <div style={{
            width: 5, height: 5, borderRadius: "50%",
            background: isLive ? "#2ea043" : "#4a4f5c",
            boxShadow: isLive ? "0 0 6px #2ea04360" : "none",
            animation: isLive ? "pulse 2s ease-in-out infinite" : "none",
            flexShrink: 0,
          }} />
          <span style={{
            fontSize: isCompact ? 8 : 9, fontWeight: 600,
            color: isLive ? "#4ade80" : "var(--theme-text-muted, #5a5f6d)",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>
            {formatTime(elapsedSec)}
          </span>
          <span style={{ fontSize: isCompact ? 8 : 9, color: "var(--theme-text-muted, #5a5f6d)" }}>·</span>
          <span style={{
            fontSize: isCompact ? 8 : 9, color: "var(--theme-text-muted, #8b8f98)",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>
            {(animBonded / 1000).toFixed(1)} Mbps
          </span>
        </div>
      </div>

      {/* ================================================================= */}
      {/* SCROLLABLE SECTIONS — flex:1 fills remaining height               */}
      {/* ================================================================= */}
      <div className="aegis-dock-scroll" style={{
        flex: 1, minHeight: 0, overflowY: "auto", overflowX: "hidden",
      }}>

        {sectionOrder.map(sectionId => {
          const isDragOver = dragOverId === sectionId && dragSrcId !== sectionId;
          const isDragging = dragSrcId === sectionId;
          const dragHandleEl = (
            <div
              draggable="true"
              onDragStart={e => { e.stopPropagation(); e.dataTransfer.effectAllowed = "move"; setDragSrcId(sectionId); }}
              onDragEnd={() => { setDragSrcId(null); setDragOverId(null); }}
              title="Drag to reorder"
              style={{
                display: "flex", alignItems: "center", justifyContent: "center",
                width: 24, cursor: "grab", flexShrink: 0,
                opacity: isDragging ? 0.8 : 0.3,
                color: "var(--theme-text-muted, #8b8f98)",
                fontSize: 14, userSelect: "none",
                transition: "opacity 0.15s",
              }}
            >
              &#8942;&#8942;
            </div>
          );
          let content = null;

          if (sectionId === "scenes") content = (
            <Section title="Scenes" icon="◉" defaultOpen={true} compact={isCompact}
              badge={activeIntent}
              badgeColor={SCENE_INTENT_COLORS[activeIntent]?.border || "#4a4f5c"}
              dragHandle={dragHandleEl}>
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
                    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                    flexShrink: 0,
                  }}>
                    Active
                  </span>
                  <span style={{ minWidth: 0, flex: 1, display: "flex", flexDirection: "column", lineHeight: 1.15 }}>
                    <span style={{
                      fontSize: isCompact ? 10 : 11,
                      color: "var(--theme-text, #e0e2e8)",
                      fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                      overflow: "hidden",
                      textOverflow: "ellipsis",
                      whiteSpace: "nowrap",
                    }}>
                      {activeSceneDisplayName}
                    </span>
                    <span style={{
                      fontSize: 8,
                      color: "var(--theme-text-muted, #8b8f98)",
                      fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
                  fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
                    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
                            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                            overflow: "hidden",
                            textOverflow: "ellipsis",
                            whiteSpace: "nowrap",
                            color: isActive ? activeRowText : "var(--theme-text, #e0e2e8)",
                          }}>
                            {row.label}
                          </span>
                          <span style={{
                            fontSize: 8,
                            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
                            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
                          style={{
                            height: 21,
                            borderRadius: 3,
                            border: "1px solid var(--theme-border, #2a2d35)",
                            background: "var(--theme-panel, #20232b)",
                            color: "var(--theme-text, #e0e2e8)",
                            fontSize: 9,
                            padding: "0 4px",
                            width: 52,
                            textAlign: "right",
                          }}
                        />
                        <span style={{ fontSize: 9, color: "var(--theme-text-muted, #8b8f98)" }}>Mbps</span>
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
                            minWidth: 0,
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
          );

          else if (sectionId === "encoders") content = (
            <Section title="Encoders & Uploads" icon="&#x229e;" defaultOpen={true} compact={isCompact}
              badge={allEncoderItems.length > 0 ? String(activeOutputCount) : "0"}
              badgeColor={activeOutputCount > 0 ? "#2ea043" : "var(--theme-border, #3a3d45)"}
              dragHandle={dragHandleEl}>
              {allEncoderItems.length === 0 && (
                <div style={{ color: "var(--theme-text-muted, #8b8f98)", fontSize: 11, padding: "8px 0", textAlign: "center" }}>
                  No encoder outputs detected
                </div>
              )}
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
              {encoderOutputs.hidden?.length > 0 && (
                <HiddenOutputsToggle items={encoderOutputs.hidden} compact={isCompact} />
              )}
            </Section>
          );

          else if (sectionId === "bitrate") content = (
            <Section title="Bitrate" icon="▥" defaultOpen={true} compact={isCompact}
              dragHandle={dragHandleEl}>
              {/* Threshold indicators */}
              <div style={{
                marginTop: 8, display: "grid", gridTemplateColumns: isUltraCompact ? "1fr" : "1fr 1fr",
                gap: 4, fontSize: 9, fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
          );

          else if (sectionId === "relay") content = (
            <Section title="Relay" icon="☁"
              compact={isCompact}
              defaultOpen={true}
              badge={relayConnections.length > 0 ? String(relayConnections.filter(c => c.status === "connected").length) + "/" + String(relayConnections.length) : "0"}
              badgeColor={relayConnections.some(c => c.status === "connected") ? "#2d7aed" : "var(--theme-border, #3a3d45)"}
              dragHandle={dragHandleEl}>
              <ConnectionListSection
                relayConnections={relayConnections}
                sendAction={sendAction}
                authAuthenticated={authAuthenticated}
                authDisplayName={authDisplayName}
                authPlanLabel={authPlanLabel}
                authPending={authPending}
                authLogin={authLogin}
                authEntitlement={authEntitlement}
                authErrorMessage={authErrorMessage}
                handleAuthLogin={handleAuthLogin}
                handleAuthLogout={handleAuthLogout}
                handleAuthOpenBrowser={handleAuthOpenBrowser}
                isCompact={isCompact}
              />
            </Section>
          );

          else if (sectionId === "failover") content = (
            <Section title="Failover Engine" icon="⚡" compact={isCompact}
              dragHandle={dragHandleEl}>
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
          );

          else if (sectionId === "quickSettings") content = (
            <Section title="Quick Settings" icon="⚙" compact={isCompact}
              dragHandle={dragHandleEl}>
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
          );

          else if (sectionId === "outputConfig") content = (
            <Section title="Output Config" icon="&#x2699;" compact={isCompact}
              badge={String(allEncoderItems.length + (encoderOutputs.hidden?.length || 0))}
              badgeColor="var(--theme-border, #3a3d45)"
              dragHandle={dragHandleEl}>
              {allEncoderItems.length === 0 ? (
                <div style={{ color: "var(--theme-text-muted, #8b8f98)", fontSize: 11, padding: "8px 0", textAlign: "center" }}>
                  No outputs configured
                </div>
              ) : (
                <OutputConfigPanel
                  encoderOutputs={encoderOutputs}
                  sendAction={sendAction}
                  compact={isCompact}
                />
              )}
            </Section>
          );

          else if (sectionId === "eventLog") content = (
            <Section title="Event Log" icon="▤" compact={isCompact}
              badge={String(events.length)} badgeColor="var(--theme-surface, #3a3d45)"
              dragHandle={dragHandleEl}>
              {events.map((e, i) => (
                <div key={e.id || i} style={{
                  display: "flex", gap: 8, padding: "4px 0",
                  borderBottom: i < events.length - 1 ? "1px solid var(--theme-border, #13151a)" : "none",
                  animation: `slideIn 0.3s ease ${i * 0.05}s both`,
                }}>
                  <span style={{
                    fontSize: 9, color: "var(--theme-text-muted, #3a3d45)",
                    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)", flexShrink: 0,
                  }}>
                    {e.time}
                  </span>
                  <span style={{
                    fontSize: 9, fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
                    color: e.type === "success" ? "#4ade80" :
                           e.type === "warning" ? "#fbbf24" :
                           e.type === "error"   ? "#da3633" : "var(--theme-text-muted, #6b7080)",
                    wordBreak: "break-word",
                  }}>{e.msg}</span>
                </div>
              ))}
            </Section>
          );

          if (!content) return null;
          return (
            <div
              key={sectionId}
              onDragOver={e => { e.preventDefault(); if (dragSrcId && dragSrcId !== sectionId) setDragOverId(sectionId); }}
              onDrop={e => {
                e.preventDefault();
                if (!dragSrcId || dragSrcId === sectionId) { setDragSrcId(null); setDragOverId(null); return; }
                setSectionOrder(prev => {
                  const next = [...prev];
                  const fromIdx = next.indexOf(dragSrcId);
                  const toIdx = next.indexOf(sectionId);
                  if (fromIdx < 0 || toIdx < 0) return prev;
                  next.splice(fromIdx, 1);
                  next.splice(toIdx, 0, dragSrcId);
                  try { window.localStorage && window.localStorage.setItem(SECTION_ORDER_STORAGE_KEY, JSON.stringify(next)); } catch (_) {}
                  return next;
                });
                setDragSrcId(null);
                setDragOverId(null);
              }}
              style={{
                outline: isDragOver ? "1px solid var(--theme-accent, #2d7aed)" : "none",
                opacity: isDragging ? 0.4 : 1,
                transition: "opacity 0.15s",
              }}
            >
              {content}
            </div>
          );
        })}

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
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
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
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>
          SIM
        </div>
      )}
    </div>
  );
}
