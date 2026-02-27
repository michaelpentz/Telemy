"use strict";

// Browser dock bootstrap exposing the plugin-call surface:
//
// Inbound (native -> dock):
//   window.aegisDockNative.receiveIpcEnvelopeJson(...)
//   window.aegisDockNative.receiveSceneSnapshotJson(...)
//   window.aegisDockNative.receivePipeStatus(...)
//   window.aegisDockNative.receiveCurrentScene(...)
//   window.aegisDockNative.receiveSceneSwitchCompletedJson(...)
//
// Outbound (dock -> native):
//   window.aegisDockNative.sendDockAction({ type, ...payload })
//   window.aegisDockNative.getState()
//   window.aegisDockNative.getCapabilities()

(function initAegisDockBrowserHostBootstrap(globalObj) {
  const g = globalObj || (typeof window !== "undefined" ? window : globalThis);
  if (!g) return;

  function ensureHost() {
    if (g.aegisDockHost && typeof g.aegisDockHost.getState === "function") {
      return g.aegisDockHost;
    }
    const exports = g.AegisDockBridgeHost;
    if (!exports || typeof exports.createWindowAegisDockBridgeHost !== "function") {
      throw new Error("AegisDockBridgeHost.createWindowAegisDockBridgeHost is not available");
    }
    return exports.createWindowAegisDockBridgeHost();
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

  function tryForwardDockActionJsonToNative(actionJson) {
    if (typeof actionJson !== "string" || !actionJson.length) return false;
    if (typeof document === "undefined" || typeof document.title !== "string") return false;
    if (typeof encodeURIComponent !== "function") return false;
    try {
      var previousTitle = String(document.title || "Aegis Dock");
      document.title = "__AEGIS_DOCK_ACTION__:" + encodeURIComponent(actionJson);
      setTimeout(function restoreTitleAfterDockActionSignal() {
        try {
          document.title = previousTitle;
        } catch (_restoreErr) {}
      }, 0);
      return true;
    } catch (_e) {
      return false;
    }
  }

  const nativeApi = {
    ensureHost,

    getState() {
      return ensureHost().getState();
    },

    receiveIpcEnvelope(envelope) {
      const ok = ensureHost().receiveIpcEnvelope(envelope);
      dispatch("aegis:dock:ipc-envelope", { ok, envelope: envelope || null });
      return ok;
    },

    receiveIpcEnvelopeJson(jsonText) {
      const ok = ensureHost().receiveIpcEnvelopeJson(jsonText);
      dispatch("aegis:dock:ipc-envelope-json", { ok });
      return ok;
    },

    receiveSceneSnapshot(payload) {
      const ok = ensureHost().setObsSceneSnapshot(payload);
      dispatch("aegis:dock:scene-snapshot", { ok, payload: payload || null });
      return ok;
    },

    receiveSceneSnapshotJson(jsonText) {
      const ok = ensureHost().setObsSceneSnapshotJson(jsonText);
      dispatch("aegis:dock:scene-snapshot-json", { ok });
      return ok;
    },

    receivePipeStatus(status, reason) {
      const ok = ensureHost().setPipeStatus(status, reason);
      dispatch("aegis:dock:pipe-status", { ok, status: status || null, reason: reason || null });
      return ok;
    },

    receiveCurrentScene(sceneName) {
      const ok = ensureHost().setObsCurrentScene(sceneName);
      dispatch("aegis:dock:current-scene", { ok, sceneName: sceneName || null });
      return ok;
    },

    receiveSceneSwitchCompleted(result) {
      const ok = ensureHost().notifySceneSwitchCompleted(result);
      dispatch("aegis:dock:scene-switch-completed", { ok, result: result || null });
      return ok;
    },

    receiveSceneSwitchCompletedJson(jsonText) {
      const ok = ensureHost().notifySceneSwitchCompletedJson(jsonText);
      dispatch("aegis:dock:scene-switch-completed-json", { ok });
      return ok;
    },

    // --- Outbound: dock UI -> bridge host ---

    sendDockAction(action) {
      var host = ensureHost();
      var hostResult = null;
      if (typeof host.sendDockAction === "function") {
        hostResult = host.sendDockAction(action);
      } else {
        dispatch("aegis:dock:action-unsupported", { action: action || null });
      }
      var jsonText = "";
      try {
        jsonText = JSON.stringify(action || {});
      } catch (_e) {
        jsonText = "";
      }
      var forwarded = tryForwardDockActionJsonToNative(jsonText);
      return hostResult;
    },

    sendDockActionJson(jsonText) {
      var parsed = parseJson(jsonText, "Invalid dock action JSON");
      if (!parsed.ok) return false;
      var host = ensureHost();
      if (typeof host.sendDockAction === "function") {
        host.sendDockAction(parsed.value);
      }
      var forwarded = tryForwardDockActionJsonToNative(String(jsonText));
      return forwarded;
    },

    getCapabilities() {
      var host = ensureHost();
      // Return capabilities object describing what the bridge supports.
      // The host may provide its own; otherwise derive from available methods.
      if (typeof host.getCapabilities === "function") {
        return host.getCapabilities();
      }
      return {
        switchScene: typeof host.sendDockAction === "function",
        setMode: typeof host.sendDockAction === "function",
        setSetting: typeof host.sendDockAction === "function",
        getState: typeof host.getState === "function",
      };
    },
  };

  g.aegisDockNative = nativeApi;
  dispatch("aegis:dock:native-ready", { ok: true });
})(typeof window !== "undefined" ? window : undefined);
