import { useState, useEffect, useRef, useCallback } from "react";
import { genRequestId } from "./utils.js";

// ---------------------------------------------------------------------------
// Bridge integration hook
// ---------------------------------------------------------------------------
// Reads DockState from the native bridge when available. Listens for bridge
// events + native action results. Falls back gracefully when bridge absent.
//
export function useDockState() {
  const [state, setState] = useState(null);
  const [bridgeAvailable, setBridgeAvailable] = useState(false);
  const actionMapRef = useRef({});

  useEffect(() => {
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
      "aegis:dock:action-native-result",
    ];

    let refresh = () => {};
    const initNativeBridge = () => {
      if (didInit) return true;
      const native = window.aegisDockNative;
      if (!native || typeof native.getState !== "function") return false;

      didInit = true;
      setBridgeAvailable(true);

      refresh = () => {
        try { setState(native.getState()); } catch (_) {}
      };

      try { setState(native.getState()); } catch (_) {}
      stateEvents.forEach(e => window.addEventListener(e, refresh));

      const host = window.aegisDockHost;
      if (host && typeof host.subscribe === "function") {
        unsub = host.subscribe(() => refresh());
      }

      poll = setInterval(refresh, 2000);
      fastPoll = setInterval(refresh, 250);
      setTimeout(() => {
        if (fastPoll) {
          clearInterval(fastPoll);
          fastPoll = null;
        }
      }, 6000);
      earlyRefresh = setTimeout(() => {
        try { setState(native.getState()); } catch (_) {}
      }, 150);
      statusRequest = setTimeout(() => {
        try {
          native.sendDockAction({ type: "request_status", requestId: genRequestId() });
        } catch (_) {}
      }, 400);

      // Self-healing: if scenes are still empty after 1s, replay the cached
      // scene snapshot from the bootstrap.  This bypasses whatever CEF IPC
      // timing issue prevents the initial delivery from reaching the bridge.
      sceneRetry = setTimeout(() => {
        try {
          const s = native.getState();
          const items = s && s.scenes && Array.isArray(s.scenes.items) ? s.scenes.items : [];
          if (items.length === 0) {
            // First try: replay the cached snapshot through the bootstrap
            if (typeof native.replayLastSceneSnapshot === "function") {
              native.replayLastSceneSnapshot();
              refresh();
            }
            // Also try the C++ request as a secondary fallback
            native.sendDockAction({ type: "request_scene_snapshot", requestId: genRequestId() });
          }
        } catch (_) {}
      }, 1000);

      // Second retry at 3s if first replay didn't work
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
        } catch (_) {}
      }, 3000);

      return true;
    };

    initNativeBridge();
    const retry = setInterval(() => {
      if (initNativeBridge()) clearInterval(retry);
    }, 250);

    return () => {
      clearInterval(retry);
      stateEvents.forEach(e => window.removeEventListener(e, refresh));
      if (unsub) unsub();
      if (poll) clearInterval(poll);
      if (fastPoll) clearInterval(fastPoll);
      if (earlyRefresh) clearTimeout(earlyRefresh);
      if (statusRequest) clearTimeout(statusRequest);
      if (sceneRetry) clearTimeout(sceneRetry);
      if (sceneRetry2) clearTimeout(sceneRetry2);
    };
  }, []);

  // Native action result tracking
  useEffect(() => {
    const handler = (e) => {
      const result = e.detail;
      if (!result?.requestId) return;
      const entry = actionMapRef.current[result.requestId];
      if (entry) {
        entry.status = result.status;
        entry.ok = result.ok;
        entry.error = result.error;
        if (result.status === "completed" || result.status === "failed" || result.status === "rejected") {
          setTimeout(() => { delete actionMapRef.current[result.requestId]; }, 3000);
        }
      }
    };
    window.addEventListener("aegis:dock:action-native-result", handler);
    return () => window.removeEventListener("aegis:dock:action-native-result", handler);
  }, []);

  const sendAction = useCallback((action) => {
    const native = window.aegisDockNative;
    if (!native || typeof native.sendDockAction !== "function") {
      console.log("[aegis-dock] sendDockAction (no native):", action);
      return null;
    }
    // Ensure requestId
    if (!action.requestId) action.requestId = genRequestId();
    // Track in-flight
    actionMapRef.current[action.requestId] = {
      type: action.type, status: "optimistic", ts: Date.now()
    };
    const result = native.sendDockAction(action);
    // Re-read state immediately (bridge may have mutated locally)
    try { setState(native.getState()); } catch (_) {}
    return result;
  }, []);

  return { state, sendAction, bridgeAvailable };
}
