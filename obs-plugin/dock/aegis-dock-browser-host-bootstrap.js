"use strict";

// v0.0.4 — Browser dock bootstrap exposing the plugin-call surface.
//
// Inbound (native -> dock):
//   window.aegisDockNative.receiveStatusSnapshotJson(...)    [NEW in v0.0.4 — primary data]
//   window.aegisDockNative.receiveSceneSnapshotJson(...)
//   window.aegisDockNative.receivePipeStatus(...)
//   window.aegisDockNative.receiveCurrentScene(...)
//   window.aegisDockNative.receiveSceneSwitchCompletedJson(...)
//   window.aegisDockNative.receiveDockActionResultJson(...)
//   window.aegisDockNative.receiveIpcEnvelopeJson(...)       [Legacy no-op for compat]
//
// Outbound (dock -> native):
//   window.aegisDockNative.sendDockAction({ type, ...payload })
//   window.aegisDockNative.getState()
//   window.aegisDockNative.getCapabilities()

(function initAegisDockBrowserHostBootstrap(globalObj) {
  var g = globalObj || (typeof window !== "undefined" ? window : globalThis);
  if (!g) return;

  function parseJsonSafe(jsonText) {
    try {
      return { ok: true, value: JSON.parse(String(jsonText)) };
    } catch (_e) {
      return { ok: false, value: null };
    }
  }

  function createFallbackHostFromBridge() {
    var bridgeApi = g.AegisDockBridge;
    if (!bridgeApi || typeof bridgeApi.createAegisDockBridgeHost !== "function") {
      return null;
    }
    var bridge = (g.__AEGIS_DOCK_BRIDGE__ && typeof g.__AEGIS_DOCK_BRIDGE__.getState === "function")
      ? g.__AEGIS_DOCK_BRIDGE__
      : bridgeApi.createAegisDockBridgeHost();
    if (typeof bridgeApi.attachAegisDockBridgeToWindow === "function") {
      bridgeApi.attachAegisDockBridgeToWindow(bridge);
    } else {
      g.__AEGIS_DOCK_BRIDGE__ = bridge;
    }

    var fallbackHost = {
      getState: function () { return bridge.getState(); },
      sendDockAction: function (action) { return bridge.sendAction(action); },

      // v0.0.4 primary data channel
      receiveStatusSnapshot: function (snapshot) {
        if (typeof bridge.receiveStatusSnapshot === "function") {
          bridge.receiveStatusSnapshot(snapshot);
          return true;
        }
        return false;
      },
      receiveStatusSnapshotJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.receiveStatusSnapshot(parsed.value) : false;
      },

      // Legacy IPC envelope — no-op in v0.0.4
      receiveIpcEnvelope: function (_envelope) {
        if (typeof bridge.handleIpcEnvelope === "function") {
          bridge.handleIpcEnvelope(_envelope);
          return true;
        }
        return false;
      },
      receiveIpcEnvelopeJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.receiveIpcEnvelope(parsed.value) : false;
      },

      // Scene snapshot
      setObsSceneSnapshot: function (payload) {
        var ok = true;
        if (typeof bridge.setObsSceneNames === "function") {
          bridge.setObsSceneNames(Array.isArray(payload && payload.sceneNames) ? payload.sceneNames : []);
        }
        if (typeof bridge.setObsActiveSceneName === "function") {
          bridge.setObsActiveSceneName(payload && payload.currentSceneName != null ? String(payload.currentSceneName) : null);
        }
        return ok;
      },
      setObsSceneSnapshotJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.setObsSceneSnapshot(parsed.value) : false;
      },
      setPipeStatus: function (status) {
        if (typeof bridge.setPipeStatus === "function") {
          bridge.setPipeStatus(status || null);
          return true;
        }
        return false;
      },
      setObsCurrentScene: function (sceneName) {
        if (typeof bridge.setObsActiveSceneName === "function") {
          bridge.setObsActiveSceneName(sceneName == null ? null : String(sceneName));
          return true;
        }
        return false;
      },
      notifySceneSwitchCompleted: function (result) {
        if (typeof bridge.notifySceneSwitchCompleted === "function") {
          bridge.notifySceneSwitchCompleted(result || {});
          return true;
        }
        return false;
      },
      notifySceneSwitchCompletedJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.notifySceneSwitchCompleted(parsed.value) : false;
      },
      notifyDockActionResult: function (result) {
        if (typeof bridge.notifyDockActionResult === "function") {
          bridge.notifyDockActionResult(result || {});
          return true;
        }
        return false;
      },
      notifyDockActionResultJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.notifyDockActionResult(parsed.value) : false;
      },
    };
    return fallbackHost;
  }

  function ensureHost() {
    if (g.aegisDockHost && typeof g.aegisDockHost.getState === "function") {
      return g.aegisDockHost;
    }
    // Prefer fallback host that directly wraps the AegisDockBridge global API.
    var fallbackHost = createFallbackHostFromBridge();
    if (fallbackHost) {
      g.aegisDockHost = fallbackHost;
      dispatch("aegis:dock:host-fallback", { ok: true, source: "AegisDockBridge" });
      return fallbackHost;
    }

    var exports = g.AegisDockBridgeHost;
    if (!exports || typeof exports.createWindowAegisDockBridgeHost !== "function") {
      throw new Error("No compatible dock bridge host is available");
    }
    try {
      var host = exports.createWindowAegisDockBridgeHost();
      g.aegisDockHost = host;
      return host;
    } catch (_e) {
      throw new Error("Failed to initialize dock bridge host");
    }
  }

  function dispatch(name, detail) {
    if (typeof g.dispatchEvent !== "function" || typeof g.CustomEvent !== "function") return;
    try {
      g.dispatchEvent(new g.CustomEvent(name, { detail: detail || {} }));
    } catch (_e) {}
  }

  function parseJson(jsonText, errorMsg) {
    try {
      return { ok: true, value: JSON.parse(String(jsonText)) };
    } catch (_e) {
      dispatch("aegis:dock:error", {
        message: errorMsg || "Invalid JSON",
        jsonText: String(jsonText),
      });
      return { ok: false, value: null };
    }
  }

  function tryForwardDockActionJsonToNativeViaTitle(actionJson) {
    if (typeof actionJson !== "string" || !actionJson.length) return false;
    if (typeof document === "undefined" || typeof document.title !== "string") return false;
    if (typeof encodeURIComponent !== "function") return false;
    try {
      var actionTitle = "__AEGIS_DOCK_ACTION__:" + encodeURIComponent(actionJson);
      document.title = actionTitle;
      return true;
    } catch (_e) {
      return false;
    }
  }

  function tryForwardDockActionJsonToNativeViaHash(actionJson) {
    if (typeof actionJson !== "string" || !actionJson.length) return false;
    if (typeof location === "undefined") return false;
    if (typeof encodeURIComponent !== "function") return false;
    try {
      var actionHash = "__AEGIS_DOCK_ACTION__:" + encodeURIComponent(actionJson);
      if (location.hash === "#" + actionHash) {
        location.hash = "";
      }
      location.hash = actionHash;
      return true;
    } catch (_e) {
      return false;
    }
  }

  function tryForwardDockActionJsonToNative(actionJson) {
    var sentViaTitle = tryForwardDockActionJsonToNativeViaTitle(actionJson);
    var sentViaHash = tryForwardDockActionJsonToNativeViaHash(actionJson);
    return sentViaTitle || sentViaHash;
  }

  function signalDockReadyToNative() {
    if (typeof encodeURIComponent !== "function") return false;
    var marker = "__AEGIS_DOCK_READY__:" + encodeURIComponent(String(Date.now()));
    var sent = false;
    try {
      if (typeof document !== "undefined" && typeof document.title === "string") {
        document.title = marker;
        sent = true;
      }
    } catch (_e) {}
    try {
      if (typeof location !== "undefined") {
        if (location.hash === "#" + marker) {
          location.hash = "";
        }
        location.hash = marker;
        sent = true;
      }
    } catch (_e) {}
    return sent;
  }

  // ── Native API surface ────────────────────────────────────────────────

  var nativeApi = {
    ensureHost: ensureHost,

    getState: function () {
      return ensureHost().getState();
    },

    // ── v0.0.4 primary data channel: status snapshot from C++ ─────────

    receiveStatusSnapshot: function (snapshot) {
      var host = ensureHost();
      var ok = false;
      if (typeof host.receiveStatusSnapshot === "function") {
        ok = host.receiveStatusSnapshot(snapshot);
      }
      dispatch("aegis:dock:status-snapshot", { ok: ok });
      return ok;
    },

    receiveStatusSnapshotJson: function (jsonText) {
      var parsed = parseJson(jsonText, "Invalid status snapshot JSON");
      if (!parsed.ok) return false;
      return nativeApi.receiveStatusSnapshot(parsed.value);
    },

    // ── Legacy IPC envelope — kept as no-op for backward compat ───────

    receiveIpcEnvelope: function (envelope) {
      var host = ensureHost();
      if (typeof host.receiveIpcEnvelope === "function") {
        host.receiveIpcEnvelope(envelope);
      }
      dispatch("aegis:dock:ipc-envelope", { ok: true, envelope: envelope || null });
      return true;
    },

    receiveIpcEnvelopeJson: function (jsonText) {
      var parsed = parseJson(jsonText, "Invalid IPC envelope JSON");
      if (!parsed.ok) return false;
      return nativeApi.receiveIpcEnvelope(parsed.value);
    },

    // ── Scene snapshot ────────────────────────────────────────────────

    receiveSceneSnapshot: function (payload) {
      var ok = ensureHost().setObsSceneSnapshot(payload);
      dispatch("aegis:dock:scene-snapshot", { ok: ok, payload: payload || null });
      return ok;
    },

    receiveSceneSnapshotJson: function (jsonText) {
      var host = ensureHost();
      var ok = false;
      if (typeof host.setObsSceneSnapshotJson === "function") {
        ok = host.setObsSceneSnapshotJson(jsonText);
      } else {
        var parsed = parseJson(jsonText, "Invalid scene snapshot JSON");
        if (parsed.ok) {
          ok = nativeApi.receiveSceneSnapshot(parsed.value);
        }
      }
      dispatch("aegis:dock:scene-snapshot-json", { ok: ok });
      return ok;
    },

    // ── Pipe status ───────────────────────────────────────────────────

    receivePipeStatus: function (status, reason) {
      var ok = ensureHost().setPipeStatus(status, reason);
      dispatch("aegis:dock:pipe-status", { ok: ok, status: status || null, reason: reason || null });
      return ok;
    },

    // ── Current scene ─────────────────────────────────────────────────

    receiveCurrentScene: function (sceneName) {
      var ok = ensureHost().setObsCurrentScene(sceneName);
      dispatch("aegis:dock:current-scene", { ok: ok, sceneName: sceneName || null });
      return ok;
    },

    // ── Scene switch completed ────────────────────────────────────────

    receiveSceneSwitchCompleted: function (result) {
      var ok = ensureHost().notifySceneSwitchCompleted(result);
      dispatch("aegis:dock:scene-switch-completed", { ok: ok, result: result || null });
      return ok;
    },

    receiveSceneSwitchCompletedJson: function (jsonText) {
      var host = ensureHost();
      var ok = false;
      if (typeof host.notifySceneSwitchCompletedJson === "function") {
        ok = host.notifySceneSwitchCompletedJson(jsonText);
      } else {
        var parsed = parseJson(jsonText, "Invalid scene-switch-completed JSON");
        if (parsed.ok) {
          ok = nativeApi.receiveSceneSwitchCompleted(parsed.value);
        }
      }
      dispatch("aegis:dock:scene-switch-completed-json", { ok: ok });
      return ok;
    },

    // ── Dock action result ────────────────────────────────────────────

    // RF-016: Allowlist of safe fields for relay_start results dispatched as CustomEvent.
    // Secrets (pair_token, ws_url, relay_ws_token, relay_shared_key) must never
    // leak into the CEF page event bus.
    _RELAY_START_SAFE_FIELDS: {
      status: 1, region: 1, public_ip: 1, srt_port: 1, relay_hostname: 1,
      grace_window_seconds: 1, max_session_seconds: 1, session_id: 1,
      provision_step: 1, slug: 1
    },

    _sanitizeRelayStartResult: function (result) {
      if (!result || typeof result !== "object") return result;
      if (result.actionType !== "relay_start") return result;

      var safeFields = nativeApi._RELAY_START_SAFE_FIELDS;
      var sanitized = {
        actionType: result.actionType,
        requestId: result.requestId,
        status: result.status,
        ok: result.ok,
        error: result.error
      };

      // Sanitize detail — only pass through allowlisted fields
      var detail = result.detail;
      if (typeof detail === "string") {
        try { detail = JSON.parse(detail); } catch (_e) { detail = null; }
      }
      if (detail && typeof detail === "object") {
        var safeDetail = {};
        for (var key in detail) {
          if (detail.hasOwnProperty(key) && safeFields[key]) {
            safeDetail[key] = detail[key];
          }
        }
        sanitized.detail = safeDetail;
      }

      return sanitized;
    },

    receiveDockActionResultJson: function (jsonText) {
      var host = ensureHost();
      var parsed = parseJson(jsonText, "Invalid dock-action-result JSON");
      var ok = false;
      if (parsed.ok) {
        if (typeof host.notifyDockActionResult === "function") {
          ok = host.notifyDockActionResult(parsed.value);
        }
        // RF-016: Sanitize relay_start results before dispatching as CustomEvent
        // to prevent secret fields from leaking into the CEF page event bus.
        var dispatchPayload = nativeApi._sanitizeRelayStartResult(parsed.value);
        dispatch("aegis:dock:action-native-result", dispatchPayload);
      }
      return ok || (parsed.ok);
    },

    // ── Outbound: dock UI -> bridge host ──────────────────────────────

    sendDockAction: function (action) {
      var host = ensureHost();
      var jsonText = "";
      try {
        jsonText = JSON.stringify(action || {});
      } catch (_e) {
        jsonText = "";
      }
      var forwarded = tryForwardDockActionJsonToNative(jsonText);
      var hostResult = null;
      if (typeof host.sendDockAction === "function") {
        try {
          hostResult = host.sendDockAction(action);
        } catch (err) {
          dispatch("aegis:dock:error", {
            message: "Host sendDockAction threw",
            error: String((err && err.message) || err || ""),
          });
        }
      } else {
        dispatch("aegis:dock:action-unsupported", { action: action || null });
      }
      return hostResult != null ? hostResult : forwarded;
    },

    sendDockActionJson: function (jsonText) {
      var parsed = parseJson(jsonText, "Invalid dock action JSON");
      if (!parsed.ok) return false;
      var forwarded = tryForwardDockActionJsonToNative(String(jsonText));
      var host = ensureHost();
      var hostOk = false;
      if (typeof host.sendDockAction === "function") {
        try {
          host.sendDockAction(parsed.value);
          hostOk = true;
        } catch (err) {
          dispatch("aegis:dock:error", {
            message: "Host sendDockActionJson threw",
            error: String((err && err.message) || err || ""),
          });
        }
      }
      return forwarded || hostOk;
    },

    getCapabilities: function () {
      var host = ensureHost();
      if (typeof host.getCapabilities === "function") {
        return host.getCapabilities();
      }
      return {
        switchScene: typeof host.sendDockAction === "function",
        setMode: typeof host.sendDockAction === "function",
        setSetting: typeof host.sendDockAction === "function",
        getState: typeof host.getState === "function",
        statusSnapshot: true,
      };
    },
  };

  g.aegisDockNative = nativeApi;
  signalDockReadyToNative();
  dispatch("aegis:dock:native-ready", { ok: true });
})(typeof window !== "undefined" ? window : undefined);
