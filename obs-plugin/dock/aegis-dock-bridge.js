"use strict";

// v0.0.4 — Thin pass-through bridge for browser-dock embedding.
// The C++ plugin now produces the full status snapshot; the bridge simply stores
// it and exposes the dock-contract getState() shape.  No IPC envelopes, no
// projection/reducer — just storage + simple mapping.
//
// Classic-script compatible (no ESM/module imports).

(function initAegisDockBridgeGlobal(globalObj) {
  const g = globalObj || (typeof window !== "undefined" ? window : globalThis);
  if (!g) return;

  // ── Utilities ────────────────────────────────────────────────────────────

  function nowMs() {
    return Date.now();
  }

  var localRequestSeq = 1;
  function newLocalRequestId(prefix) {
    return String(prefix || "ui") + "-" + String(Date.now()) + "-" + String(localRequestSeq++);
  }

  function pushRing(list, item, max) {
    list.unshift(item);
    if (list.length > max) list.length = max;
  }

  function formatHms(tsUnixMs) {
    if (!tsUnixMs) return "--:--:--";
    var d = new Date(tsUnixMs);
    var hh = String(d.getHours()).padStart(2, "0");
    var mm = String(d.getMinutes()).padStart(2, "0");
    var ss = String(d.getSeconds()).padStart(2, "0");
    return hh + ":" + mm + ":" + ss;
  }

  function resolveSceneIdByName(scenes, sceneName) {
    if (!sceneName || !Array.isArray(scenes)) return null;
    for (var i = 0; i < scenes.length; i += 1) {
      var s = scenes[i];
      if (s && s.name === sceneName) return s.id || null;
    }
    var target = String(sceneName).toLowerCase();
    for (var i = 0; i < scenes.length; i += 1) {
      var s = scenes[i];
      if (s && String(s.name || "").toLowerCase() === target) return s.id || null;
    }
    return null;
  }

  function normalizeOutputBitrates(raw) {
    if (!Array.isArray(raw)) return [];
    return raw
      .map(function (item, idx) {
        if (!item || typeof item !== "object") return null;
        var platform = item.platform || item.name || item.host || ("Output " + String(idx + 1));
        var kbpsRaw = item.kbps != null ? item.kbps : (item.bitrate_kbps != null ? item.bitrate_kbps : item.bitrate);
        var kbps = Number(kbpsRaw);
        return {
          platform: String(platform),
          kbps: Number.isFinite(kbps) ? kbps : null,
          status: item.status || (item.active ? "active" : "inactive"),
        };
      })
      .filter(Boolean);
  }

  function buildEncoderOutputs(raw) {
    if (!Array.isArray(raw) || raw.length === 0) return { groups: [], hidden: [] };
    var groupMap = {};
    var groupOrder = [];
    var hidden = [];
    for (var i = 0; i < raw.length; i++) {
      var item = raw[i];
      if (!item || typeof item !== "object") continue;
      var normalized = {
        id: item.id || ("output-" + String(i + 1)),
        name: item.name || item.platform || ("Output " + String(i + 1)),
        platform: item.platform || item.name || ("Output " + String(i + 1)),
        active: item.active !== false,
        kbps: Number(item.bitrate_kbps != null ? item.bitrate_kbps : (item.kbps != null ? item.kbps : 0)),
        fps: item.fps != null ? Number(item.fps) : null,
        dropPct: item.drop_pct != null ? Number(item.drop_pct) : null,
        encodingLagMs: item.encoding_lag_ms != null ? Number(item.encoding_lag_ms) : null,
        encoder: item.encoder || null,
        resolution: item.resolution || null,
      };
      if (item.hidden) {
        hidden.push(normalized);
        continue;
      }
      var groupKey = item.group || "Ungrouped";
      if (!groupMap[groupKey]) {
        groupMap[groupKey] = { name: groupKey, resolution: item.resolution || null, items: [], totalBitrateKbps: 0, avgLagMs: null };
        groupOrder.push(groupKey);
      }
      groupMap[groupKey].items.push(normalized);
      groupMap[groupKey].totalBitrateKbps += (normalized.kbps || 0);
      if (!groupMap[groupKey].resolution && item.resolution) {
        groupMap[groupKey].resolution = item.resolution;
      }
    }
    var groups = [];
    for (var gi = 0; gi < groupOrder.length; gi++) {
      var g = groupMap[groupOrder[gi]];
      var lagSum = 0; var lagCount = 0;
      for (var li = 0; li < g.items.length; li++) {
        if (g.items[li].encodingLagMs != null) { lagSum += g.items[li].encodingLagMs; lagCount++; }
      }
      g.avgLagMs = lagCount > 0 ? Math.round(lagSum / lagCount * 10) / 10 : null;
      groups.push(g);
    }
    return { groups: groups, hidden: hidden };
  }

  function sanitizeTheme(theme) {
    return theme && typeof theme === "object" ? Object.assign({}, theme) : null;
  }

  // ── Default settings (used when C++ hasn't sent settings yet) ────────────

  var DEFAULT_SETTINGS = [
    { key: "auto_scene_switch", label: "Auto Scene Switch", value: null, color: "#2ea043" },
    { key: "low_quality_fallback", label: "Low Bitrate Fallback", value: null, color: "#d29922" },
    { key: "manual_override", label: "Manual Override", value: null, color: "#5ba3f5" },
    { key: "chat_bot", label: "Chat Bot Integration", value: null, color: "#8b8f98" },
    { key: "alerts", label: "Alert on Disconnect", value: null, color: "#2d7aed" },
  ];

  // ── Bridge Host Factory ──────────────────────────────────────────────────

  function createAegisDockBridgeHost(options) {
    var listeners = new Set();
    var cfg = Object.assign({ eventLimit: 50 }, options || {});

    // Core state — the C++ status snapshot (raw from receiveStatusSnapshotJson)
    var statusSnapshot = null;

    // Plugin-side state — populated by direct C++ calls (scenes, pipe, etc.)
    var plugin = {
      scenes: [],
      activeSceneId: null,
      activeSceneName: null,
      pendingScene: null,
      pipeForcedStatus: null,
      connections: null,
      settings: null,
      isLive: null,
      elapsedSec: null,
      relayCache: null, // cached relay session data from relay_start action result
      relayError: null, // last relay_start error (cleared on next relay_start attempt)
      relayErrorTs: 0,  // timestamp of last relay error
      relayProvisionStep: null, // { step, stepNumber, totalSteps, label }
      prevPktLoss: 0,   // previous pktLoss for delta rate calculation
      prevPktLossTs: 0, // timestamp of previous pktLoss sample
      lossRate: 0,      // packets lost per second (smoothed)
    };

    // Event ring buffer
    var events = [];

    // Theme cache (survives snapshot updates that lack a theme field)
    var lastTheme = null;

    // Failover transition tracking
    var failoverTransitionCount = 0;
    var lastFailoverTs = null;
    var lastFailoverState = null;

    // ── Helpers ──────────────────────────────────────────────────────────

    function addEvent(type, msg, tsUnixMs) {
      if (!msg) return;
      pushRing(events, {
        id: String((tsUnixMs || nowMs())) + "-" + Math.random().toString(16).slice(2, 8),
        time: formatHms(tsUnixMs || nowMs()),
        type: type || "info",
        msg: String(msg),
      }, cfg.eventLimit);
    }

    function emit() {
      var state = buildState();
      listeners.forEach(function (fn) {
        try { fn(state); } catch (_e) {}
      });
    }

    // ── State builder (replaces projectDockState) ───────────────────────
    // Maps the C++ status snapshot + plugin-side state into the dock contract.

    function buildState() {
      var snap = statusSnapshot || {};
      var relay = snap.relay || {};

      // Merge cached relay session data (from relay_start action result)
      // Cache wins for status when snapshot says "inactive" but cache says otherwise
      // (covers the provisioning gap before heartbeat confirms active)
      if (plugin.relayCache) {
        for (var rk in plugin.relayCache) {
          if (plugin.relayCache[rk] != null) {
            if (!relay[rk] || (rk === "status" && relay[rk] === "inactive" && plugin.relayCache[rk] !== "inactive")) {
              relay[rk] = plugin.relayCache[rk];
            }
          }
        }
      }

      // Scenes
      var scenes = Array.isArray(plugin.scenes) ? plugin.scenes : [];
      var activeSceneId = plugin.activeSceneId || resolveSceneIdByName(scenes, plugin.activeSceneName) || null;
      var pendingSceneId =
        (plugin.pendingScene && plugin.pendingScene.sceneId) ||
        resolveSceneIdByName(scenes, plugin.pendingScene && plugin.pendingScene.sceneName) ||
        null;

      // Pipe status — simplified: C++ sets forced status, default is "ok" when snapshot exists
      var pipeStatus = plugin.pipeForcedStatus || (statusSnapshot ? "ok" : "down");
      var pipeLabel = pipeStatus === "ok" ? "PIPE OK" : (pipeStatus === "degraded" ? "PIPE DEGRADED" : "PIPE DOWN");

      // Settings — prefer plugin-provided settings, fall back to defaults (clone to avoid mutation)
      var settingsItems = (Array.isArray(plugin.settings) && plugin.settings.length)
        ? plugin.settings
        : DEFAULT_SETTINGS.map(function(s) { return Object.assign({}, s); });

      // Overlay C++ snapshot settings (flat object) onto the settings array
      // so that persisted config values are reflected even when plugin.settings is null
      var snapSettings = snap.settings;
      if (snapSettings && typeof snapSettings === "object") {
        for (var si2 = 0; si2 < settingsItems.length; si2++) {
          var sk = settingsItems[si2].key;
          if (sk && typeof snapSettings[sk] === "boolean") {
            settingsItems[si2] = Object.assign({}, settingsItems[si2], { value: snapSettings[sk] });
          }
        }
      }

      // Determine auto-switch armed from settings
      var autoSwitchEnabled = null;
      var manualOverrideEnabled = false;
      for (var i = 0; i < settingsItems.length; i++) {
        var si = settingsItems[i];
        if (si.key === "auto_scene_switch" && typeof si.value === "boolean") {
          autoSwitchEnabled = si.value;
        }
        if (si.key === "manual_override" && typeof si.value === "boolean") {
          manualOverrideEnabled = si.value;
        }
      }
      var autoSwitchArmed = manualOverrideEnabled
        ? false
        : (typeof autoSwitchEnabled === "boolean" ? autoSwitchEnabled : true);

      // Health
      var health = snap.health || "offline";

      // Mode
      var mode = snap.mode || "studio";

      // Failover transition tracking — detect state changes
      var currentFailoverState = snap.state_mode || (mode === "irl" ? "IRL_ACTIVE" : "STUDIO");
      if (lastFailoverState !== null && currentFailoverState !== lastFailoverState) {
        failoverTransitionCount++;
        lastFailoverTs = nowMs();
      }
      lastFailoverState = currentFailoverState;

      // Theme
      var snapshotTheme = sanitizeTheme(snap.theme);
      if (snapshotTheme) {
        lastTheme = snapshotTheme;
      }
      var theme = snapshotTheme || lastTheme;

      // Live status — prefer plugin override, fall back to snapshot health
      var isLive = typeof plugin.isLive === "boolean" ? plugin.isLive : (health !== "offline");
      var elapsedSec = plugin.elapsedSec || 0;

      // ── Build the dock-contract state ───────────────────────────────

      var state = {
        header: {
          mode: mode,
          version: "v0.0.4",
        },
        live: {
          isLive: isLive,
          elapsedSec: elapsedSec,
        },
        scenes: {
          items: scenes,
          activeSceneId: activeSceneId,
          pendingSceneId: pendingSceneId,
          autoSwitchArmed: autoSwitchArmed,
          autoSwitchEnabled: autoSwitchEnabled,
          manualOverrideEnabled: manualOverrideEnabled,
        },
        connections: {
          items: Array.isArray(plugin.connections) ? plugin.connections : [],
        },
        bitrate: {
          bondedKbps: Number(snap.bitrate_kbps || 0),
          relayBondedKbps: Number(
            snap.relay_bonded_kbps ||
            (relay.bonded_kbps || relay.ingest_bonded_kbps) ||
            snap.bitrate_kbps ||
            0
          ),
          outputs: normalizeOutputBitrates(snap.multistream_outputs || []),
          lowThresholdMbps: snap.low_threshold_mbps != null ? Number(snap.low_threshold_mbps) : null,
          brbThresholdMbps: snap.brb_threshold_mbps != null ? Number(snap.brb_threshold_mbps) : null,
        },
        outputs: buildEncoderOutputs(snap.multistream_outputs || []),
        relay: {
          enabled: (mode === "irl") || !!(relay.status && relay.status !== "inactive"),
          active: !!(relay.status && relay.status !== "inactive" && relay.status !== "stopped" && relay.status !== "provisioning"),
          status: relay.status || "inactive",
          region: relay.region || null,
          latencyMs: snap.relay_rtt_ms != null ? snap.relay_rtt_ms : (snap.rtt_ms != null ? snap.rtt_ms : null),
          graceRemainingSeconds: relay.grace_remaining_seconds != null
            ? relay.grace_remaining_seconds
            : null,
          // Connection credentials (nested relay obj OR top-level fallback)
          publicIp: relay.public_ip || snap.relay_public_ip || null,
          srtPort: relay.srt_port || snap.relay_srt_port || 5000,
          relayHostname: relay.relay_hostname || snap.relay_hostname || null,
          ingestUrl: (relay.relay_hostname || snap.relay_hostname || relay.public_ip || snap.relay_public_ip)
            ? ("srtla://" + (relay.relay_hostname || snap.relay_hostname || relay.public_ip || snap.relay_public_ip) + ":" + String(relay.srt_port || snap.relay_srt_port || 5000))
            : null,
          wsUrl: relay.ws_url || null,
          graceWindowSeconds: relay.grace_window_seconds || null,
          maxSessionSeconds: relay.max_session_seconds || null,
          licensed: true, // placeholder — future: OAuth gate derives from subscription status
          lastError: plugin.relayError || null,
          lastErrorTs: plugin.relayErrorTs || 0,
          relayProvisionStep: plugin.relayProvisionStep || null,
          // SLS relay telemetry (aggregate bonded stream)
          // Compute loss rate (pkt/s) from cumulative delta
          lossRate: (function() {
            var loss = snap.relay_pkt_loss || 0;
            var now = Date.now();
            var dt = (now - plugin.prevPktLossTs) / 1000;
            if (dt > 0.5 && plugin.prevPktLossTs > 0) {
              var delta = loss - plugin.prevPktLoss;
              plugin.lossRate = dt > 0 ? Math.max(0, Math.round(delta / dt)) : 0;
            }
            plugin.prevPktLoss = loss;
            plugin.prevPktLossTs = now;
            return plugin.lossRate;
          })(),
          ingestBitrateKbps: snap.relay_ingest_bitrate_kbps || 0,
          rttMs: snap.relay_rtt_ms != null ? snap.relay_rtt_ms : null,
          pktLoss: snap.relay_pkt_loss || 0,
          pktDrop: snap.relay_pkt_drop || 0,
          recvRateMbps: snap.relay_recv_rate_mbps != null ? snap.relay_recv_rate_mbps : null,
          bandwidthMbps: snap.relay_bandwidth_mbps != null ? snap.relay_bandwidth_mbps : null,
          relayLatencyMs: snap.relay_latency_ms != null ? snap.relay_latency_ms : null,
          uptimeSeconds: snap.relay_uptime_seconds || 0,
          statsAvailable: !!snap.relay_stats_available,
          // Per-link telemetry (from srtla_rec fork)
          perLinkAvailable: !!snap.relay_per_link_available,
          connCount: snap.relay_conn_count || 0,
          links: (snap.relay_links || []).map(function(l) {
            return {
              addr: l.addr || "",
              bytes: l.bytes || 0,
              pkts: l.pkts || 0,
              sharePct: l.share_pct || 0,
              lastMsAgo: l.last_ms_ago || 0,
              uptimeS: l.uptime_s || 0,
              asn_org: l.asn_org || "",
            };
          }),
        },
        failover: {
          health: health,
          state: currentFailoverState,
          responseBudgetMs: 800,
          lastFailoverLabel: lastFailoverTs ? formatHms(lastFailoverTs) : null,
          totalFailoversLabel: failoverTransitionCount > 0 ? String(failoverTransitionCount) : null,
        },
        settings: {
          items: settingsItems,
        },
        events: events.slice(0, 12),
        pipe: {
          status: pipeStatus,
          label: pipeLabel,
        },
        _bridge: {
          protocolVersion: null,
          protocolErrorsRecent: 0,
          lastSnapshotTs: snap._ts || null,
          compat: "v0.0.4-passthrough",
        },
      };

      if (theme) {
        state.theme = theme;
      }

      return state;
    }

    // ── Public API ──────────────────────────────────────────────────────

    return {
      getState: function () {
        return buildState();
      },

      // ── Inbound: C++ status snapshot (primary data channel) ─────────

      receiveStatusSnapshot: function (snapshot) {
        if (!snapshot || typeof snapshot !== "object") return;
        snapshot._ts = nowMs();
        statusSnapshot = snapshot;
        emit();
      },

      // ── Inbound: C++ scene snapshot ─────────────────────────────────

      setObsSceneNames: function (sceneNames) {
        var names = Array.isArray(sceneNames) ? sceneNames : [];
        plugin.scenes = names.map(function (name, index) {
          return { id: "scene-" + String(index + 1), name: String(name) };
        });
        if (plugin.activeSceneName) {
          plugin.activeSceneId = resolveSceneIdByName(plugin.scenes, plugin.activeSceneName);
        }
        emit();
      },

      setObsActiveSceneName: function (sceneName) {
        plugin.activeSceneName = sceneName == null ? null : String(sceneName);
        plugin.activeSceneId = resolveSceneIdByName(plugin.scenes, plugin.activeSceneName);
        if (plugin.pendingScene && plugin.pendingScene.sceneName && plugin.activeSceneName &&
            plugin.pendingScene.sceneName === plugin.activeSceneName) {
          plugin.pendingScene = null;
        }
        emit();
      },

      // ── Inbound: C++ scene switch result ────────────────────────────

      notifySceneSwitchCompleted: function (arg) {
        var result = arg || {};
        var ok = !!result.ok;
        if (plugin.pendingScene) {
          plugin.pendingScene = null;
        }
        addEvent(ok ? "success" : "error",
          ok
            ? ("Scene switch applied" + (result.sceneName ? ": " + String(result.sceneName) : ""))
            : ("Scene switch failed" + (result.error ? ": " + String(result.error) : "")));
        emit();
      },

      // ── Inbound: C++ dock action result ─────────────────────────────

      notifyDockActionResult: function (result) {
        if (!result || typeof result !== "object") return;
        var msg = "Action " + String(result.actionType || "unknown") + ": " + String(result.status || "unknown");
        addEvent(result.ok ? "success" : (result.status === "rejected" ? "warning" : "error"), msg);

        // Track relay errors in bridge state (dock reads via getState())
        if (result.actionType === "relay_start" && !result.ok) {
          plugin.relayError = result.error || result.status || "relay_start_failed";
          plugin.relayErrorTs = nowMs();
          plugin.relayProvisionStep = null;
        }
        if (result.actionType === "relay_start" && result.ok) {
          plugin.relayError = null;
          plugin.relayErrorTs = 0;
          // Don't clear relayProvisionStep here — let it persist until relayActive hides the block
        }
        // Handle provision progress events embedded in action results
        if (result.actionType === "relay_provision_progress") {
          // detail comes as a JSON string from C++ — parse it
          var pd = result;
          if (typeof result.detail === "string" && result.detail) {
            try { pd = JSON.parse(result.detail); } catch (_e) {}
          } else if (typeof result.detail === "object" && result.detail) {
            pd = result.detail;
          }
          plugin.relayProvisionStep = {
            step: pd.step || "",
            stepNumber: pd.stepNumber || 0,
            totalSteps: pd.totalSteps || 6,
            label: pd.label || "",
          };
        }

        // Cache relay session data from relay_start result for immediate UI update
        if (result.actionType === "relay_start" && result.ok && result.detail) {
          var d = result.detail;
          if (typeof d === "string") { try { d = JSON.parse(d); } catch(_) { d = null; } }
          if (d) {
            plugin.relayCache = {
              status: d.status || "active",
              region: d.region || null,
              public_ip: d.public_ip || null,
              srt_port: d.srt_port || 9000,
              relay_hostname: d.relay_hostname || null,
              pair_token: d.pair_token || null,
              ws_url: d.ws_url || null,
              grace_window_seconds: d.grace_window_seconds || null,
              max_session_seconds: d.max_session_seconds || null,
            };
          }
        }
        // Clear relay cache on stop
        if (result.actionType === "relay_stop") {
          plugin.relayCache = null;
        }

        emit();
      },

      // ── Inbound: pipe status ────────────────────────────────────────

      setPipeStatus: function (status) {
        plugin.pipeForcedStatus = status || null;
        emit();
      },

      // ── Inbound: connection telemetry ───────────────────────────────

      setConnectionTelemetry: function (items) {
        plugin.connections = Array.isArray(items) ? items.slice() : [];
        emit();
      },

      // ── Inbound: settings ───────────────────────────────────────────

      setSettings: function (items) {
        plugin.settings = Array.isArray(items) ? items.slice() : [];
        emit();
      },

      // ── Inbound: live info ──────────────────────────────────────────

      setLiveInfo: function (info) {
        var v = info || {};
        if (typeof v.isLive === "boolean") plugin.isLive = v.isLive;
        if (typeof v.elapsedSec === "number") plugin.elapsedSec = v.elapsedSec;
        emit();
      },

      // ── Outbound: dock UI -> native ─────────────────────────────────

      sendAction: async function (action) {
        if (!action || !action.type) return false;
        if (action.type === "switch_scene") {
          plugin.pendingScene = {
            requestId: newLocalRequestId("manual"),
            sceneName: action.sceneName || null,
            sceneId: action.sceneId || resolveSceneIdByName(plugin.scenes, action.sceneName),
            tsUnixMs: nowMs(),
          };
          addEvent("info", "Manual scene switch queued: " + String(action.sceneName || action.sceneId || "unknown"));
          emit();
          return true;
        }
        if (action.type === "relay_start" || action.type === "relay_stop") {
          addEvent("info", "Relay action forwarded to native: " + String(action.type));
          emit();
          return true;
        }
        addEvent("info", "Unhandled dock action: " + String(action.type));
        emit();
        return false;
      },

      // ── Subscribe to state changes ──────────────────────────────────

      subscribe: function (listener) {
        if (typeof listener !== "function") return function noop() {};
        listeners.add(listener);
        return function unsubscribe() {
          listeners.delete(listener);
        };
      },

      // ── Event push (for host adapter compatibility) ─────────────────

      pushEvent: function (evt) {
        if (!evt) return;
        addEvent(evt.type || "info", evt.msg || evt.message || "Event", evt.ts);
      },

      // ── Legacy compatibility stubs ──────────────────────────────────
      // handleIpcEnvelope is removed in v0.0.4 — no more IPC envelopes.
      // These stubs prevent errors if old code still calls them.

      handleIpcEnvelope: function (_envelope) {
        // No-op in v0.0.4.  Status data comes via receiveStatusSnapshot().
      },

      setBitrateThresholds: function (_thresholds) {
        // No-op in v0.0.4.  Thresholds are part of settings.
      },

      setEngineState: function (_engineState) {
        // No-op in v0.0.4.  Engine state is derived from snapshot.state_mode.
      },
    };
  }

  function attachAegisDockBridgeToWindow(bridge, key) {
    g[key || "__AEGIS_DOCK_BRIDGE__"] = bridge;
    return bridge;
  }

  var exported = {
    createAegisDockBridgeHost: createAegisDockBridgeHost,
    attachAegisDockBridgeToWindow: attachAegisDockBridgeToWindow,
  };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = exported;
  }
  g.AegisDockBridge = exported;
})(typeof window !== "undefined" ? window : undefined);
