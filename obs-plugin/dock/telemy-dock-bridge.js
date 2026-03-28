"use strict";

// v0.0.5 — Thin pass-through bridge for browser-dock embedding.
// The C++ plugin now produces the full status snapshot; the bridge simply stores
// it and exposes the dock-contract getState() shape.  No IPC envelopes, no
// projection/reducer — just storage + simple mapping.
//
// Classic-script compatible (no ESM/module imports).

(function initTelemyDockBridgeGlobal(globalObj) {
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

  // Clean up verbose OBS encoder instance names for display.
  // obs_encoder_get_name() returns internal names like "streaming_h264",
  // "advanced_video_recording", "ADVANCED_VIDEO_STREAMING VIDEO ENCODER", etc.
  function cleanEncoderName(raw, codec) {
    if (!raw) return codec ? codec.toUpperCase() : "Encoder";
    var s = String(raw);
    // Strip trailing " VIDEO ENCODER", " ENCODER" (case-insensitive)
    s = s.replace(/\s*VIDEO\s*ENCODER\s*$/i, "").replace(/\s*ENCODER\s*$/i, "");
    // Clean known OBS internal prefixes
    s = s.replace(/^advanced[_\s]+video[_\s]+/i, "");
    s = s.replace(/^simple[_\s]+/i, "");
    s = s.replace(/^streaming[_\s]+/i, "");
    s = s.replace(/^recording[_\s]+/i, "");
    // Replace underscores with spaces
    s = s.replace(/_/g, " ");
    // Collapse whitespace
    s = s.replace(/\s+/g, " ").trim();
    // If we stripped everything, fall back to codec or original
    if (!s) s = codec ? codec.toUpperCase() : String(raw);
    return s;
  }

  // Clean up OBS output names — translate internal names to user-friendly labels.
  // Most multi-stream plugins (StreamElements, Aitum, obs-multi-rtmp) set
  // user-visible names already. This handles native OBS internal output names.
  function cleanOutputName(raw) {
    if (!raw) return null;
    var s = String(raw);
    // Known OBS internal output names
    var knownNames = {
      "adv_stream": "Stream",
      "simple_stream": "Stream",
      "default_service": "Stream",
      "adv_file_output": "Recording",
      "simple_file_output": "Recording",
      "adv_stream2": "Stream 2",
      "replay_buffer": "Replay Buffer",
      "virtualcam_output": "Virtual Camera",
    };
    var lower = s.toLowerCase();
    if (knownNames[lower]) return knownNames[lower];
    // Strip "- Output" / " Output" suffix common in some plugins
    s = s.replace(/\s*-?\s*[Oo]utput\s*$/, "");
    // Replace underscores with spaces
    s = s.replace(/_/g, " ").replace(/\s+/g, " ").trim();
    return s || raw;
  }

  function buildEncoderOutputs(raw) {
    if (!Array.isArray(raw) || raw.length === 0) return { groups: [], hidden: [] };
    var groupMap = {};
    var groupOrder = [];
    var hidden = [];
    for (var i = 0; i < raw.length; i++) {
      var item = raw[i];
      if (!item || typeof item !== "object") continue;
      // Prefer display_name from C++ multi-stream plugin resolution (e.g. StreamElements),
      // then fall back to cleaned OBS output name.
      var displayName = item.display_name || cleanOutputName(item.name) || item.platform || ("Output " + String(i + 1));
      var normalized = {
        id: item.id || ("output-" + String(i + 1)),
        name: displayName,
        platform: item.platform || displayName,
        active: item.active !== false,
        isPrimary: !!item.is_primary,
        kbps: Number(item.bitrate_kbps != null ? item.bitrate_kbps : (item.kbps != null ? item.kbps : 0)),
        targetKbps: item.target_bitrate_kbps != null ? Number(item.target_bitrate_kbps) : (item.targetKbps != null ? Number(item.targetKbps) : 0),
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
      // Derive group from encoder name + resolution when no explicit group is set.
      // Group KEY includes resolution to distinguish same-encoder-different-canvas.
      // Group DISPLAY NAME prefers canvas_name (from SE), then cleaned encoder name.
      var resolution = item.resolution || (item.width && item.height ? (item.width + "x" + item.height) : null);
      var cleanedEncoder = cleanEncoderName(item.encoder, item.codec);
      var groupKey = item.group || (item.encoder && resolution ? (item.encoder + "|" + resolution) : (item.encoder || "Ungrouped"));
      var groupDisplayName = item.group || item.canvas_name || (item.is_primary ? "Native Canvas" : cleanedEncoder);
      normalized.resolution = resolution;
      if (!groupMap[groupKey]) {
        groupMap[groupKey] = { name: groupDisplayName, resolution: resolution, items: [], totalBitrateKbps: 0, avgLagMs: null };
        groupOrder.push(groupKey);
      }
      groupMap[groupKey].items.push(normalized);
      groupMap[groupKey].totalBitrateKbps += (normalized.kbps || 0);
      if (!groupMap[groupKey].resolution && resolution) {
        groupMap[groupKey].resolution = resolution;
      }
    }
    var groups = [];
    // Rename groups that contain a primary output to "Native Canvas"
    // (the primary output may not be the first item processed for its group)
    var hasPrimary = function(key) {
      var items = groupMap[key].items;
      for (var pi = 0; pi < items.length; pi++) { if (items[pi].isPrimary) return true; }
      return false;
    };
    for (var ki = 0; ki < groupOrder.length; ki++) {
      if (hasPrimary(groupOrder[ki]) && !groupMap[groupOrder[ki]].name.match(/canvas/i)) {
        groupMap[groupOrder[ki]].name = "Native Canvas";
      }
    }
    // Sort groups: groups containing a primary output come first
    groupOrder.sort(function(a, b) {
      var ap = hasPrimary(a) ? 0 : 1;
      var bp = hasPrimary(b) ? 0 : 1;
      return ap - bp;
    });
    for (var gi = 0; gi < groupOrder.length; gi++) {
      var g = groupMap[groupOrder[gi]];
      // Sort items within group: primary first
      g.items.sort(function(a, b) { return (a.isPrimary ? 0 : 1) - (b.isPrimary ? 0 : 1); });
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

  function createTelemyDockBridgeHost(options) {
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
      authState: null,
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
      var auth = (snap.auth && typeof snap.auth === "object") ? Object.assign({}, snap.auth) : {};
      if ((!auth || Object.keys(auth).length === 0) && plugin.authState && typeof plugin.authState === "object") {
        auth = Object.assign({}, plugin.authState);
      }

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

      // Live status — prefer plugin override, then obs_stats.streaming, then health fallback
      var obsStats = snap.obs_stats || {};
      var obsStreaming = obsStats.streaming === true;
      var isLive = typeof plugin.isLive === "boolean" ? plugin.isLive : (obsStreaming || health !== "offline");

      // Elapsed time — prefer plugin override, then derive from C++ streaming_start_ms
      // (C++ tracks the true start time so elapsed survives dock reloads mid-stream)
      var streamingStartMs = obsStats.streaming_start_ms || 0;
      var elapsedSec = plugin.elapsedSec || (streamingStartMs > 0 ? Math.floor((nowMs() - streamingStartMs) / 1000) : 0);

      // ── Build the dock-contract state ───────────────────────────────

      var state = {
        header: {
          mode: mode,
          version: "v0.0.5",
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
        relay_connections: (Array.isArray(snap.relay_connections) ? snap.relay_connections : []).map(function(rc) {
          if (!rc || typeof rc !== "object") return rc;
          // Normalize flat stats_* fields (from C++ snapshot) into a nested stats object,
          // and compute loss_pct for display.
          var existingStats = rc.stats && typeof rc.stats === "object" ? rc.stats : null;
          var statsAvailable = existingStats ? existingStats.available : (rc.stats_available === true);
          var statsBitrateKbps = existingStats ? existingStats.bitrate_kbps : (rc.stats_bitrate_kbps || 0);
          var statsRttMs = existingStats ? existingStats.rtt_ms : (rc.stats_rtt_ms || 0);
          var statsPktLoss = existingStats ? (existingStats.pkt_loss || 0) : (rc.stats_pkt_loss || 0);
          var statsPktRecv = existingStats ? (existingStats.pkt_recv || 0) : (rc.stats_pkt_recv || 0);
          var statsPktTotal = statsPktLoss + statsPktRecv;
          var statsLossPct = statsPktTotal > 0 ? statsPktLoss / statsPktTotal * 100 : 0;
          return Object.assign({}, rc, {
            stats: {
              available: statsAvailable,
              bitrate_kbps: statsBitrateKbps,
              rtt_ms: statsRttMs,
              pkt_loss: statsPktLoss,
              pkt_recv: statsPktRecv,
              loss_pct: statsLossPct,
            },
          });
        }),
      connections: {
          items: Array.isArray(plugin.connections)
            ? plugin.connections
            : (Array.isArray(snap.connections) ? snap.connections.map(function (item, idx) {
                if (!item || typeof item !== "object") return null;
                return {
                  id: item.id || ("conn-" + String(idx + 1)),
                  name: item.name || ("Link " + String(idx + 1)),
                  type: item.type || "Unknown",
                  signal: Number(item.signal != null ? item.signal : 0),
                  bitrate: Number(item.bitrate != null ? item.bitrate : (item.bitrate_kbps != null ? item.bitrate_kbps : 0)),
                  status: item.status || "disconnected",
                };
              }).filter(Boolean) : []),
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
          byorEnabled: !!(snapSettings && snapSettings.byor_enabled === true),
          byorRelayHost: (snapSettings && snapSettings.byor_relay_host != null) ? String(snapSettings.byor_relay_host) : "",
          byorRelayPort: (snapSettings && snapSettings.byor_relay_port != null) ? Number(snapSettings.byor_relay_port) : 5000,
          byorStreamId: (snapSettings && snapSettings.byor_stream_id != null) ? String(snapSettings.byor_stream_id) : "",
          region: relay.region || null,
          latencyMs: snap.relay_rtt_ms != null ? snap.relay_rtt_ms : (snap.rtt_ms != null ? snap.rtt_ms : null),
          graceRemainingSeconds: relay.grace_remaining_seconds != null
            ? relay.grace_remaining_seconds
            : null,
          // Connection credentials (nested relay obj OR top-level fallback)
          publicIp: relay.public_ip || snap.relay_public_ip || null,
          srtPort: relay.srt_port || snap.relay_srt_port || 5000,
          relayHostname: relay.relay_hostname || snap.relay_hostname || null,
          streamToken: null, // stripped from dock state for security — C++ handles tokens internally
          ingestUrl: (relay.relay_hostname || snap.relay_hostname || relay.public_ip || snap.relay_public_ip)
            ? ("srtla://" + (relay.relay_hostname || snap.relay_hostname || relay.public_ip || snap.relay_public_ip) + ":" + String(relay.srt_port || snap.relay_srt_port || 5000))
            : null,
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
          pktRecv: snap.relay_pkt_recv || 0,
          lossPct: (function() {
            var loss = snap.relay_pkt_loss || 0;
            var recv = snap.relay_pkt_recv || 0;
            var total = loss + recv;
            return total > 0 ? loss / total * 100 : 0;
          })(),
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
        auth: {
          authenticated: auth.authenticated === true,
          hasTokens: auth.has_tokens === true,
          user: auth.user || null,
          entitlement: auth.entitlement || null,
          usage: auth.usage || null,
          stream_slots: auth.stream_slots || [],
          activeRelay: auth.active_relay || null,
          linked_accounts: auth.linked_accounts || null,
          login: auth.login || { pending: false, poll_interval_seconds: 3 },
          lastErrorCode: auth.last_error_code || null,
          lastErrorMessage: auth.last_error_message || null,
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
        chatbot: (snap.chatbot && typeof snap.chatbot === "object")
          ? Object.assign({}, snap.chatbot)
          : null,
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
        if (result.actionType === "chatbot_simulate_message" && result.detail) {
          var cd = result.detail;
          if (typeof cd === "string") { try { cd = JSON.parse(cd); } catch (_e) { cd = null; } }
          if (cd && typeof cd === "object") {
            if (cd.reply_text) {
              msg = "Chatbot: " + String(cd.reply_text);
            } else if (cd.scene_name) {
              msg = "Chatbot -> " + String(cd.scene_name);
            } else if (cd.error_code) {
              msg = "Chatbot rejected: " + String(cd.error_code);
            }
          }
        }
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
              srt_port: d.srt_port || 5000,
              relay_hostname: d.relay_hostname || null,
              grace_window_seconds: d.grace_window_seconds || null,
              max_session_seconds: d.max_session_seconds || null,
            };
          }
        }
        // Clear relay cache on stop
        if (result.actionType === "relay_stop") {
          plugin.relayCache = null;
        }

        if (result.actionType && result.actionType.indexOf("auth_") === 0) {
          if (result.detail) {
            var authDetail = result.detail;
            if (typeof authDetail === "string") {
              try { authDetail = JSON.parse(authDetail); } catch (_e) { authDetail = null; }
            }
            if (authDetail && typeof authDetail === "object") {
              if (result.actionType === "auth_login_start" && result.ok) {
                var currentAuth = (plugin.authState && typeof plugin.authState === "object") ? plugin.authState : {};
                var currentLogin = (currentAuth.login && typeof currentAuth.login === "object") ? currentAuth.login : {};
                plugin.authState = Object.assign({}, currentAuth, {
                  login: Object.assign({}, currentLogin, {
                    pending: true,
                    login_attempt_id: authDetail.login_attempt_id || currentLogin.login_attempt_id || "",
                    authorize_url: authDetail.authorize_url || currentLogin.authorize_url || "",
                    expires_at: authDetail.expires_at || currentLogin.expires_at || "",
                    poll_interval_seconds: authDetail.poll_interval_seconds || currentLogin.poll_interval_seconds || 3,
                  }),
                  last_error_code: null,
                  last_error_message: null,
                });
              } else {
                plugin.authState = authDetail;
              }
            }
          }
          if (result.actionType === "auth_logout" && result.ok) {
            plugin.authState = {
              authenticated: false,
              has_tokens: false,
              user: null,
              entitlement: null,
              usage: null,
              active_relay: null,
              login: { pending: false, poll_interval_seconds: 3 },
              last_error_code: null,
              last_error_message: null,
            };
          }
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
        if (
          action.type === "relay_start" ||
          action.type === "relay_stop" ||
          action.type === "relay_connect_direct" ||
          action.type === "relay_disconnect_direct" ||
          (action.type && action.type.indexOf("auth_") === 0) ||
          (action.type && action.type.indexOf("connection_") === 0)
        ) {
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

  function attachTelemyDockBridgeToWindow(bridge, key) {
    g[key || "__TELEMY_DOCK_BRIDGE__"] = bridge;
    return bridge;
  }

  var exported = {
    createTelemyDockBridgeHost: createTelemyDockBridgeHost,
    attachTelemyDockBridgeToWindow: attachTelemyDockBridgeToWindow,
  };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = exported;
  }
  g.TelemyDockBridge = exported;
})(typeof window !== "undefined" ? window : undefined);
