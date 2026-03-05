"use strict";

// v0.0.4 — Host-side adapter around aegis-dock-bridge.js for browser-dock integration.
// Defines stable entry points for plugin/local callbacks and status snapshot delivery.
// IPC envelope support retained as no-op stubs for backward compatibility.

var bridgeModule = null;

function loadBridgeModule() {
  if (bridgeModule) return bridgeModule;

  if (typeof window !== "undefined" && window.AegisDockBridge) {
    bridgeModule = window.AegisDockBridge;
    return bridgeModule;
  }

  if (typeof require === "function") {
    try {
      bridgeModule = require("./aegis-dock-bridge.js");
      return bridgeModule;
    } catch (_err) {
      // In embedded browser contexts a `require` symbol may exist but not be usable for local assets.
      // Fall through to the final error so browser-global injection can be preferred when available.
    }
  }

  throw new Error("aegis-dock-bridge module not available");
}

function createAegisDockBridgeHost(options) {
  var opts = options || {};
  var bridgeApi = loadBridgeModule();
  var bridge = opts.bridge || bridgeApi.createAegisDockBridgeHost(opts.bridgeOptions || {});
  var listeners = new Set();

  function emit(eventName, payload) {
    listeners.forEach(function (fn) {
      try {
        fn(eventName, payload, bridge.getState());
      } catch (_err) {
        // Swallow listener errors to avoid breaking dock updates.
      }
    });
  }

  function normalizeSceneSnapshot(payload) {
    if (!payload || typeof payload !== "object") return null;
    var names = Array.isArray(payload.sceneNames)
      ? payload.sceneNames
      : Array.isArray(payload.scenes)
      ? payload.scenes
      : [];
    return {
      reason: payload.reason || "unknown",
      sceneNames: names.map(function (s) { return typeof s === "string" ? s : (s && s.name) || ""; }).filter(Boolean),
      currentSceneName:
        payload.currentSceneName || payload.current_scene_name || payload.activeSceneName || null,
    };
  }

  var host = {
    bridge: bridge,

    getState: function () {
      return bridge.getState();
    },

    subscribe: function (listener) {
      if (typeof listener !== "function") return function noop() {};
      listeners.add(listener);
      return function unsubscribe() {
        listeners.delete(listener);
      };
    },

    subscribeDockState: function (listener) {
      return bridge.subscribe(listener);
    },

    // ── v0.0.4 primary data channel: status snapshot ──────────────────

    receiveStatusSnapshot: function (snapshot) {
      if (typeof bridge.receiveStatusSnapshot === "function") {
        bridge.receiveStatusSnapshot(snapshot);
      }
      emit("status_snapshot", snapshot || {});
      return true;
    },

    receiveStatusSnapshotJson: function (jsonText) {
      try {
        var snapshot = JSON.parse(String(jsonText));
        return host.receiveStatusSnapshot(snapshot);
      } catch (_err) {
        if (typeof bridge.pushEvent === "function") {
          bridge.pushEvent({
            source: "bridge-host",
            type: "error",
            msg: "Invalid status snapshot JSON",
          });
        }
        emit("status_snapshot_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    // ── Legacy IPC envelope — no-op pass-through ──────────────────────

    receiveIpcEnvelope: function (envelope) {
      if (typeof bridge.handleIpcEnvelope === "function") {
        bridge.handleIpcEnvelope(envelope);
      }
      emit("ipc_envelope", envelope);
      return true;
    },

    receiveIpcEnvelopeJson: function (jsonText) {
      try {
        var envelope = JSON.parse(String(jsonText));
        return host.receiveIpcEnvelope(envelope);
      } catch (_err) {
        if (typeof bridge.pushEvent === "function") {
          bridge.pushEvent({
            source: "bridge-host",
            type: "error",
            msg: "Invalid IPC envelope JSON",
          });
        }
        emit("ipc_envelope_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    // ── Scene snapshot ────────────────────────────────────────────────

    setObsSceneSnapshot: function (payload) {
      var snap = normalizeSceneSnapshot(payload);
      if (!snap) return false;
      bridge.setObsSceneNames(snap.sceneNames);
      if (snap.currentSceneName !== null) {
        bridge.setObsActiveSceneName(snap.currentSceneName);
      }
      if (typeof bridge.pushEvent === "function") {
        bridge.pushEvent({
          source: "obs",
          type: "info",
          msg:
            "OBS scene snapshot (" +
            snap.reason +
            "): " +
            String(snap.sceneNames.length) +
            " scenes" +
            (snap.currentSceneName ? ", current=" + snap.currentSceneName : ""),
        });
      }
      emit("obs_scene_snapshot", snap);
      return true;
    },

    setObsSceneSnapshotJson: function (jsonText) {
      try {
        var payload = JSON.parse(String(jsonText));
        return host.setObsSceneSnapshot(payload);
      } catch (_err) {
        if (typeof bridge.pushEvent === "function") {
          bridge.pushEvent({
            source: "bridge-host",
            type: "error",
            msg: "Invalid OBS scene snapshot JSON",
          });
        }
        emit("obs_scene_snapshot_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    setObsCurrentScene: function (sceneName) {
      bridge.setObsActiveSceneName(sceneName || null);
      emit("obs_current_scene", { sceneName: sceneName || null });
      return true;
    },

    // ── Pipe status ───────────────────────────────────────────────────

    setPipeStatus: function (status, reason) {
      bridge.setPipeStatus(status || null);
      emit("pipe_status", { status: status, reason: reason || null });
      return true;
    },

    // ── Connection telemetry ──────────────────────────────────────────

    setConnectionTelemetry: function (items) {
      bridge.setConnectionTelemetry(items || []);
      emit("connection_telemetry", { count: Array.isArray(items) ? items.length : 0 });
      return true;
    },

    // ── Live info ─────────────────────────────────────────────────────

    setLiveInfo: function (liveInfo) {
      bridge.setLiveInfo(liveInfo || {});
      emit("live_info", liveInfo || {});
      return true;
    },

    // ── Settings ──────────────────────────────────────────────────────

    setSettings: function (settings) {
      bridge.setSettings(settings || {});
      emit("settings", settings || {});
      return true;
    },

    // ── Engine state (legacy no-op) ───────────────────────────────────

    setEngineState: function (engineState) {
      if (typeof bridge.setEngineState === "function") {
        bridge.setEngineState(engineState || null);
      }
      emit("engine_state", { engineState: engineState || null });
      return true;
    },

    // ── Bitrate thresholds (legacy no-op) ─────────────────────────────

    setBitrateThresholds: function (thresholds) {
      if (typeof bridge.setBitrateThresholds === "function") {
        bridge.setBitrateThresholds(thresholds || {});
      }
      emit("bitrate_thresholds", thresholds || {});
      return true;
    },

    // ── Scene switch completed ────────────────────────────────────────

    notifySceneSwitchCompleted: function (result) {
      bridge.notifySceneSwitchCompleted(result || {});
      emit("scene_switch_completed", result || {});
      return true;
    },

    notifySceneSwitchCompletedJson: function (jsonText) {
      try {
        var result = JSON.parse(String(jsonText));
        return host.notifySceneSwitchCompleted(result);
      } catch (_err) {
        if (typeof bridge.pushEvent === "function") {
          bridge.pushEvent({
            source: "bridge-host",
            type: "error",
            msg: "Invalid scene-switch-completed JSON",
          });
        }
        emit("scene_switch_completed_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    // ── Dock action result ────────────────────────────────────────────

    notifyDockActionResult: function (result) {
      if (typeof bridge.notifyDockActionResult === "function") {
        bridge.notifyDockActionResult(result || {});
      }
      emit("dock_action_result", result || {});
      return true;
    },

    notifyDockActionResultJson: function (jsonText) {
      try {
        var result = JSON.parse(String(jsonText));
        return host.notifyDockActionResult(result);
      } catch (_err) {
        if (typeof bridge.pushEvent === "function") {
          bridge.pushEvent({
            source: "bridge-host",
            type: "error",
            msg: "Invalid dock-action-result JSON",
          });
        }
        emit("dock_action_result_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    // ── Outbound dock action ──────────────────────────────────────────

    sendDockAction: function (action) {
      var out = bridge.sendAction(action);
      emit("dock_action", { action: action, result: out });
      return out;
    },
  };

  return host;
}

function attachAegisDockBridgeHostToWindow(targetWindow, host, options) {
  var win = targetWindow || (typeof window !== "undefined" ? window : null);
  if (!win) return null;
  var opts = options || {};
  var key = opts.key || "aegisDockHost";
  win[key] = host;
  return host;
}

function createWindowAegisDockBridgeHost(options) {
  var host = createAegisDockBridgeHost(options);
  if (typeof window !== "undefined") {
    attachAegisDockBridgeHostToWindow(window, host, options);
  }
  return host;
}

var exported = {
  createAegisDockBridgeHost: createAegisDockBridgeHost,
  createWindowAegisDockBridgeHost: createWindowAegisDockBridgeHost,
  attachAegisDockBridgeHostToWindow: attachAegisDockBridgeHostToWindow,
};

if (typeof module !== "undefined" && module.exports) {
  module.exports = exported;
}

if (typeof window !== "undefined") {
  window.AegisDockBridgeHost = exported;
}
