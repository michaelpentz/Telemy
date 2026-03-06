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
  ConnectionCard, EngineStateChips
} from "./ui-components.jsx";
import { SceneButton } from "./scene-components.jsx";
import {
  OutputBar, EncoderGroupHeader, HiddenOutputsToggle,
  OutputConfigRow, OutputConfigPanel
} from "./encoder-components.jsx";

// =============================================================================
// MAIN DOCK COMPONENT
// =============================================================================

export default function AegisDock() {
  const dockRootRef = useRef(null);
  const dockLayout = useDockCompactMode(dockRootRef);
  const isCompact = dockLayout === "compact" || dockLayout === "ultra";
  const isUltraCompact = dockLayout === "ultra";
  const prevLinkBytesRef = useRef({});
  const prevLinkTsRef = useRef(0);

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
  const relayActive = relay.active === true;
  const relayLicensed = relay.licensed !== false; // default true for backward compat
  const derivedMode = relayActive && conns.length > 0 ? "irl" : "studio";

  // Relay activation UI state
  const [relayActivating, setRelayActivating] = useState(false);
  const [relayDeactivating, setRelayDeactivating] = useState(false);
  const [relayError, setRelayError] = useState(null);

  // Clear activating spinner when relay becomes active
  useEffect(() => {
    if (relayActive && relayActivating) {
      setRelayActivating(false);
      setRelayError(null);
    }
  }, [relayActive, relayActivating]);

  // Clear deactivating spinner when relay becomes inactive
  useEffect(() => {
    if (!relayActive && relayDeactivating) {
      setRelayDeactivating(false);
    }
  }, [relayActive, relayDeactivating]);

  // Handle failed relay action results — read from bridge state (relay.lastError)
  // and also listen for DOM events as a backup
  const relayLastError = relay.lastError || null;
  const relayLastErrorTs = relay.lastErrorTs || 0;
  const relayErrorTsRef = useRef(0);
  useEffect(() => {
    if (relayLastError && relayLastErrorTs > relayErrorTsRef.current) {
      relayErrorTsRef.current = relayLastErrorTs;
      setRelayActivating(false);
      setRelayError(relayLastError);
    }
  }, [relayLastError, relayLastErrorTs]);

  // DOM event backup for relay errors
  useEffect(() => {
    const handler = (e) => {
      const r = e.detail;
      if (!r) return;
      if (r.actionType === "relay_start" && r.ok === false) {
        setRelayActivating(false);
        setRelayError(r.error || r.status || "Relay start failed");
      }
      if (r.actionType === "relay_stop" && r.ok === false) {
        setRelayError(r.error || "Relay stop failed");
      }
    };
    window.addEventListener("aegis:dock:action-native-result", handler);
    return () => window.removeEventListener("aegis:dock:action-native-result", handler);
  }, []);

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
  // Per-link throughput from cumulative byte deltas
  const perLinkThroughputs = useMemo(() => {
    if (!relay.perLinkAvailable || !relay.links || relay.links.length === 0) return [];
    const now = Date.now();
    const dt = (now - prevLinkTsRef.current) / 1000;
    const prev = prevLinkBytesRef.current;
    const results = relay.links.map(link => {
      const prevBytes = prev[link.addr] || 0;
      const kbps = dt > 0.5 && prevBytes > 0
        ? Math.max(0, ((link.bytes - prevBytes) / dt) * 8 / 1000)
        : 0;
      return { ...link, kbps, ...classifyLinkAddr(link.addr) };
    });
    const nextPrev = {};
    relay.links.forEach(l => { nextPrev[l.addr] = l.bytes; });
    prevLinkBytesRef.current = nextPrev;
    prevLinkTsRef.current = now;
    return results;
  }, [relay.perLinkAvailable, relay.links]);
  // Relay connection URLs (for copy-to-clipboard)
  const relayIngestHost = relay.relayHostname || relay.publicIp;
  const relayIngestUrl = relayIngestHost ? "srtla://" + relayIngestHost + ":" + (relay.srtPort || 5000) : null;
  const relayObsPlayUrl = relayIngestHost ? "srt://" + relayIngestHost + ":4000?streamid=play_aegis" : null;
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
    if (relayActivating || relayDeactivating) return;
    if (relayActive) {
      setRelayDeactivating(true);
      sendAction({ type: "relay_stop" });
    } else {
      setRelayActivating(true);
      setRelayError(null);
      sendAction({ type: "relay_start" });
      // Poll bridge state for relay error/success — CEF event delivery is unreliable
      // Relay provisioning takes ~20s API + ~3 min bootstrap; 90s timeout covers it
      const pollStart = Date.now();
      const pollId = setInterval(() => {
        const native = window.aegisDockNative;
        const s = native && typeof native.getState === "function" ? native.getState() : null;
        const r = s && s.relay;
        if (r && r.lastError) {
          clearInterval(pollId);
          setRelayActivating(false);
          setRelayError(r.lastError);
          return;
        }
        if (r && r.active) {
          clearInterval(pollId);
          setRelayActivating(false);
          setRelayError(null);
          return;
        }
        if (Date.now() - pollStart > 90000) {
          clearInterval(pollId);
          setRelayActivating(false);
          setRelayError("Activation timed out — check relay status");
        }
      }, 500);
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
  const version = header.version || "v0.0.4";
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
          {/* Per-link bars from srtla_rec stats */}
          {relayActive && perLinkThroughputs.length > 0 ? (
            <>
              {perLinkThroughputs.map((link, i) => (
                <div key={link.addr} style={{ opacity: link.lastMsAgo > 3000 ? 0.4 : 1 }}>
                  <BitrateBar
                    value={link.kbps}
                    max={Math.max(relayBondedKbps, 6000)}
                    color={i === 0 ? "#2d7aed" : i === 1 ? "#8b5cf6" : "#e05d44"}
                    label={`${link.label}  ${Math.round(link.sharePct)}%`}
                  />
                </div>
              ))}
              <BitrateBar value={relayBondedKbps} max={Math.max(relayBondedKbps * 1.5, 10000)} color="#2ea043" label="BONDED" />
            </>
          ) : relayActive && conns.length >= 2 ? (
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
        <Section title="Encoders & Uploads" icon="&#x229e;" defaultOpen={true} compact={isCompact}
          badge={allEncoderItems.length > 0 ? String(activeOutputCount) : "0"}
          badgeColor={activeOutputCount > 0 ? "#2ea043" : "var(--theme-border, #3a3d45)"}>
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

        {/* ----- RELAY (always visible, state-machine driven) ----- */}
        <Section title="Relay" icon="☁"
          compact={isCompact}
          defaultOpen={true}
          badge={!relayLicensed ? "PRO" : relayActive ? (relay.connCount > 0 ? `${relay.connCount} LINK${relay.connCount !== 1 ? "S" : ""}` : relayStatusUi.toUpperCase()) : "OFF"}
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

          {/* --- IDLE state (licensed, not active, not provisioning) --- */}
          {relayLicensed && !relayActive && !relayActivating && !relayError && relay.status !== "provisioning" && (
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

          {/* --- ACTIVATING state (explicit activating OR bridge reports provisioning) --- */}
          {relayLicensed && (relayActivating || relay.status === "provisioning") && !relayActive && (
            <div style={{
              background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "10px 10px",
              border: "1px solid #d2992240", textAlign: "center",
            }}>
              <div style={{ fontSize: 10, color: "#d29922", fontWeight: 600 }}>
                Provisioning relay&hellip; this may take a few minutes
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
                    {formatTime(relay.uptimeSeconds || 0)}
                  </span>
                </div>
              </div>

              {/* Relay Stream Quality (from SLS stats) */}
              {relay.statsAvailable && (
                <div style={{
                  background: "var(--theme-surface, #13151a)", borderRadius: 4,
                  padding: "8px 10px", marginBottom: 6,
                  border: "1px solid var(--theme-border, #1e2028)",
                }}>
                  <div style={{ fontSize: 9, fontWeight: 700, textTransform: "uppercase",
                    color: "var(--theme-text-muted, #8b8f98)", marginBottom: 6,
                    letterSpacing: "0.5px" }}>
                    Relay Ingest
                  </div>
                  {/* Bitrate bar */}
                  <div style={{ marginBottom: 4 }}>
                    <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 2 }}>
                      <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)" }}>Bitrate</span>
                      <span style={{ fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontWeight: 600 }}>
                        {relay.ingestBitrateKbps >= 1000
                          ? `${(relay.ingestBitrateKbps / 1000).toFixed(1)} Mbps`
                          : `${relay.ingestBitrateKbps} kbps`}
                      </span>
                    </div>
                    <div style={{ height: 4, borderRadius: 2,
                      background: "var(--theme-border, #3a3d45)", overflow: "hidden" }}>
                      <div style={{
                        height: "100%", borderRadius: 2, transition: "width 0.6s ease",
                        width: `${Math.min(100, relay.bandwidthMbps > 0
                          ? ((relay.recvRateMbps || 0) / relay.bandwidthMbps) * 100 : 50)}%`,
                        background: relay.bandwidthMbps > 0 && (relay.recvRateMbps || 0) / relay.bandwidthMbps > 0.9
                          ? "#da3633" : relay.bandwidthMbps > 0 && (relay.recvRateMbps || 0) / relay.bandwidthMbps > 0.7
                          ? "#d29922" : "#2ea043",
                      }} />
                    </div>
                  </div>
                  {/* Stats pills — row 1: network */}
                  <div style={{ display: "flex", gap: 8, marginTop: 6 }}>
                    <StatPill label="RTT" value={relay.rttMs != null ? `${Math.round(relay.rttMs)}ms` : "\u2014"}
                      color={relay.rttMs > 100 ? "#da3633" : relay.rttMs > 50 ? "#d29922" : "#2ea043"} />
                    <StatPill label="Latency" value={relay.relayLatencyMs != null ? `${relay.relayLatencyMs}ms` : "\u2014"}
                      color={relay.relayLatencyMs > 3500 ? "#da3633" : relay.relayLatencyMs > 2500 ? "#d29922" : "#2ea043"} />
                  </div>
                  {/* Stats pills — row 2: packet loss */}
                  <div style={{ display: "flex", gap: 8, marginTop: 4 }}>
                    <StatPill label="Loss" value={relay.pktLoss > 0 ? relay.pktLoss.toLocaleString() : "0"}
                      color={relay.pktLoss > 1000 ? "#da3633" : relay.pktLoss > 100 ? "#d29922" : "#2ea043"} />
                    <StatPill label="Loss/s" value={relay.lossRate > 0 ? String(relay.lossRate) : "0"}
                      color={relay.lossRate > 50 ? "#da3633" : relay.lossRate > 10 ? "#d29922" : "#2ea043"} />
                    <StatPill label="Drop" value={relay.pktDrop > 0 ? relay.pktDrop.toLocaleString() : "0"}
                      color={relay.pktDrop > 1000 ? "#da3633" : relay.pktDrop > 100 ? "#d29922" : "#2ea043"} />
                  </div>
                </div>
              )}

              {/* Connection URLs — flat siblings, click-to-copy */}
              {relayIngestUrl && <div style={{ fontSize: 9, color: "var(--theme-text, #e0e2e8)", padding: "3px 10px", cursor: "pointer", fontFamily: "monospace", background: "var(--theme-surface, #13151a)", borderTop: "1px solid var(--theme-border, #2a2d35)", borderLeft: "1px solid var(--theme-border, #2a2d35)", borderRight: "1px solid var(--theme-border, #2a2d35)", borderRadius: "4px 4px 0 0", marginTop: 2 }} onClick={function() { cefCopyToClipboard(relayIngestUrl); }}>{"Ingest  " + relayIngestUrl + "  \u29C9"}</div>}
              {relayIngestUrl && <div style={{ fontSize: 9, color: "var(--theme-text, #e0e2e8)", padding: "3px 10px", cursor: "pointer", fontFamily: "monospace", background: "var(--theme-surface, #13151a)", borderLeft: "1px solid var(--theme-border, #2a2d35)", borderRight: "1px solid var(--theme-border, #2a2d35)" }} onClick={function() { cefCopyToClipboard("live_aegis"); }}>{"Stream   live_aegis  \u29C9"}</div>}
              {relayObsPlayUrl && <div style={{ fontSize: 9, color: "var(--theme-text, #e0e2e8)", padding: "3px 10px", cursor: "pointer", fontFamily: "monospace", background: "var(--theme-surface, #13151a)", borderBottom: "1px solid var(--theme-border, #2a2d35)", borderLeft: "1px solid var(--theme-border, #2a2d35)", borderRight: "1px solid var(--theme-border, #2a2d35)", borderRadius: "0 0 4px 4px", marginBottom: 6 }} onClick={function() { cefCopyToClipboard(relayObsPlayUrl); }}>{"Play     " + relayObsPlayUrl + "  \u29C9"}</div>}

              {/* Deactivate button */}
              {relayDeactivating ? (
                <div style={{
                  width: "100%", padding: "5px 0", textAlign: "center",
                  borderRadius: 3, background: "var(--theme-panel, #20232b)",
                  border: "1px solid #d2992240",
                  color: "#d29922", fontSize: 9, fontWeight: 600,
                  fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                  marginBottom: 6,
                }}>
                  Deactivating relay&hellip;
                </div>
              ) : (
                <button onClick={handleRelayToggle} style={{
                  width: "100%", padding: "5px 0", border: "1px solid var(--theme-border, #2a2d35)",
                  borderRadius: 3, background: "var(--theme-panel, #20232b)", cursor: "pointer",
                  color: "var(--theme-text-muted, #5a5f6d)", fontSize: 9, fontWeight: 500,
                  fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
                  marginBottom: 6,
                }}>
                  Deactivate Relay
                </button>
              )}

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

        {/* ----- OUTPUT CONFIG (only when outputs exist) ----- */}
        {allEncoderItems.length > 0 && (
          <Section title="Output Config" icon="&#x2699;" compact={isCompact}
            badge={String(allEncoderItems.length + (encoderOutputs.hidden?.length || 0))}
            badgeColor="var(--theme-border, #3a3d45)">
            <OutputConfigPanel
              encoderOutputs={encoderOutputs}
              sendAction={sendAction}
              compact={isCompact}
            />
          </Section>
        )}

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
