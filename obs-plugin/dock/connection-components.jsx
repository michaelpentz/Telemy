import { useState } from "react";
import { StatusDot, BitrateBar, ConnectionTypeBadge, SecretField } from "./ui-components.jsx";
import { cefCopyToClipboard, genRequestId } from "./utils.js";

// =============================================================================
// CONNECTION LIST COMPONENTS  (v0.0.5 multi-connection model)
// =============================================================================

const MANAGED_REGIONS = [
  { id: "us-east",     label: "US East" },
  { id: "us-west",     label: "US West" },
  { id: "eu-central",  label: "EU Central" },
  { id: "ap-southeast", label: "AP Southeast" },
];

function connStatusColor(status) {
  if (status === "connected")  return "#2ea043";
  if (status === "connecting") return "#d29922";
  if (status === "error")      return "#da3633";
  return "#5a5f6d";
}

// --- Animated dots for provisioning ---
function ProvisionDots() {
  return (
    <span>
      <span style={{ animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0s" }}>.</span>
      <span style={{ animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0.2s" }}>.</span>
      <span style={{ animation: "dotBlink 1.4s ease-in-out infinite", animationDelay: "0.4s" }}>.</span>
    </span>
  );
}

// --- Inline provision progress (replaces old full-screen modal) ---
function ManagedProvisionProgress({ step }) {
  const hasStep = step && step.stepNumber > 0;
  const pct = hasStep ? Math.round((step.stepNumber / step.totalSteps) * 100) : 15;
  return (
    <div style={{
      marginTop: 4, padding: "5px 8px",
      background: "rgba(210,153,34,0.07)", borderRadius: 3,
      border: "1px solid #d2992230",
    }}>
      <div style={{
        display: "flex", justifyContent: "space-between",
        fontSize: 9, color: "#d29922", marginBottom: 3,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
      }}>
        <span>{hasStep ? step.label : "Starting relay"}<ProvisionDots /></span>
        {hasStep && <span>{pct}%</span>}
      </div>
      <div style={{ height: 3, borderRadius: 2, background: "var(--theme-border, #2a2d35)", overflow: "hidden" }}>
        <div style={{
          height: "100%", width: pct + "%", borderRadius: 2,
          background: "#d29922", transition: "width 0.4s ease",
        }} />
      </div>
    </div>
  );
}

// --- Expanded detail: secrets + per-link + edit/remove ---
function ConnectionExpandedDetail({ conn, sendAction, onRemove }) {
  const isByor = conn.type === "byor";
  const [isEditing, setIsEditing] = useState(false);
  const [editName, setEditName] = useState(conn.name || "");
  const [editHost, setEditHost] = useState(conn.relay_host_masked || conn.relay_host || "");
  const [editPort, setEditPort] = useState(String(conn.relay_port || 5000));
  const [editStreamId, setEditStreamId] = useState(conn.stream_id || "");
  const [editRegion, setEditRegion] = useState(conn.managed_region || "us-east");

  const inputStyle = {
    width: "100%", height: 21, borderRadius: 3,
    border: "1px solid var(--theme-border, #2a2d35)",
    background: "var(--theme-panel, #20232b)",
    color: "var(--theme-text, #e0e2e8)",
    fontSize: 9, padding: "0 6px",
    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
    boxSizing: "border-box",
  };

  const labelStyle = {
    fontSize: 8, color: "var(--theme-text-muted, #8b8f98)",
    textTransform: "uppercase", letterSpacing: "0.04em", marginBottom: 2,
    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
  };

  const handleSave = () => {
    const payload = {
      type: "connection_update", id: conn.id, requestId: genRequestId(),
      name: editName.trim(),
    };
    if (isByor) {
      payload.relay_host = editHost.trim();
      payload.relay_port = parseInt(editPort, 10) || 5000;
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
    setEditPort(String(conn.relay_port || 5000));
    setEditStreamId(conn.stream_id || "");
    setEditRegion(conn.managed_region || "us-east");
    setIsEditing(false);
  };

  if (isEditing) {
    return (
      <div style={{
        marginTop: 6, padding: "8px 8px",
        background: "var(--theme-surface, #13151a)", borderRadius: 4,
        border: "1px solid var(--theme-border, #2a2d35)",
      }}>
        <div style={{ marginBottom: 6 }}>
          <div style={labelStyle}>Name</div>
          <input
            value={editName}
            onChange={e => setEditName(e.target.value)}
            style={inputStyle}
          />
        </div>
        {isByor && (
          <div>
            <div style={{ marginBottom: 6 }}>
              <div style={labelStyle}>Relay Host</div>
              <input
                value={editHost}
                onChange={e => setEditHost(e.target.value)}
                placeholder="relay.example.com"
                style={inputStyle}
              />
            </div>
            <div style={{ display: "grid", gridTemplateColumns: "70px minmax(0, 1fr)", gap: 6, marginBottom: 6 }}>
              <div>
                <div style={labelStyle}>Port</div>
                <input
                  value={editPort}
                  onChange={e => setEditPort(e.target.value.replace(/[^\d]/g, "").slice(0, 5))}
                  inputMode="numeric"
                  placeholder="5000"
                  style={inputStyle}
                />
              </div>
              <div>
                <div style={labelStyle}>Stream ID</div>
                <input
                  value={editStreamId}
                  onChange={e => setEditStreamId(e.target.value)}
                  placeholder="live/stream"
                  style={inputStyle}
                />
              </div>
            </div>
          </div>
        )}
        {!isByor && (
          <div style={{ marginBottom: 6 }}>
            <div style={labelStyle}>Region</div>
            <select
              value={editRegion}
              onChange={e => setEditRegion(e.target.value)}
              style={{ ...inputStyle, height: 23 }}
            >
              {MANAGED_REGIONS.map(r => <option key={r.id} value={r.id}>{r.label}</option>)}
            </select>
          </div>
        )}
        <div style={{ display: "flex", gap: 6 }}>
          <button onClick={handleSave} style={{
            flex: 1, height: 22, borderRadius: 3, border: "none",
            background: "#2d7aed", color: "#fff", fontSize: 9, fontWeight: 600,
            cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>Save</button>
          <button onClick={handleCancelEdit} style={{
            height: 22, padding: "0 10px", borderRadius: 3,
            border: "1px solid var(--theme-border, #2a2d35)",
            background: "var(--theme-panel, #20232b)",
            color: "var(--theme-text-muted, #8b8f98)", fontSize: 9, cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>Cancel</button>
        </div>
      </div>
    );
  }

  return (
    <div style={{ marginTop: 6 }}>
      <div style={{
        padding: "8px 8px", background: "var(--theme-surface, #13151a)",
        borderRadius: 4, border: "1px solid var(--theme-border, #2a2d35)",
        marginBottom: 4,
      }}>
        {isByor && (
          <div>
            <SecretField label="Host" value={conn.relay_host_masked || conn.relay_host || ""} copyValue={conn.relay_host || conn.relay_host_masked || ""} />
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 5 }}>
              <span style={labelStyle}>Port</span>
              <span style={{ fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }}>
                {conn.relay_port || 5000}
              </span>
            </div>
            <SecretField label="Stream ID" value={conn.stream_id || ""} copyValue={conn.stream_id || ""} />
          </div>
        )}
        {!isByor && (
          <div>
            <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 5, fontSize: 9 }}>
              <span style={{ color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }}>Region</span>
              <span style={{ color: "var(--theme-text, #e0e2e8)", fontWeight: 600, fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }}>{conn.managed_region || "\u2014"}</span>
            </div>
            {conn.session_id && (
              <SecretField label="Session ID" value={conn.session_id} copyValue={conn.session_id} />
            )}
            {conn.relay_ip && (
              <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 5, fontSize: 9 }}>
                <span style={{ color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }}>Relay IP</span>
                <span style={{ color: "var(--theme-text, #e0e2e8)", fontWeight: 600, fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)" }}>{conn.relay_ip}</span>
              </div>
            )}
          </div>
        )}
        {conn.status === "error" && conn.error_msg && (
          <div style={{
            fontSize: 9, color: "#da3633", marginTop: 4, padding: "4px 6px",
            background: "rgba(218,54,51,0.08)", borderRadius: 3,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>
            {conn.error_msg}
          </div>
        )}
      </div>

      <div style={{ display: "flex", gap: 5 }}>
        <button onClick={() => setIsEditing(true)} style={{
          flex: 1, height: 22, borderRadius: 3,
          border: "1px solid var(--theme-border, #2a2d35)",
          background: "var(--theme-panel, #20232b)",
          color: "var(--theme-text-muted, #8b8f98)", fontSize: 9, cursor: "pointer",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>Edit</button>
        <button onClick={onRemove} style={{
          height: 22, padding: "0 10px", borderRadius: 3,
          border: "1px solid #da363340",
          background: "rgba(218,54,51,0.06)",
          color: "#da3633", fontSize: 9, cursor: "pointer",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>Remove</button>
      </div>
    </div>
  );
}

// --- Compute aggregate bitrate + RTT for a connected relay (used in header + bars) ---
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
  const mbps = (totalKbps / 1000).toFixed(1);
  return { mbps, rttMs };
}

// --- Mini stacked health bar shown inline in collapsed connection row ---
function MiniHealthBar({ conn, onClick, isExpanded }) {
  if (conn.status !== "connected") return null;

  const hasPerLink = conn.per_link?.available && Array.isArray(conn.per_link?.links) && conn.per_link.links.length > 0;
  let segments = [];

  if (hasPerLink) {
    const links = conn.per_link.links;
    const totalKbps = links.reduce((s, l) => s + (l.bitrate_kbps || 0), 0);
    segments = links.map(l => {
      const kbps = l.bitrate_kbps || 0;
      const pct = totalKbps > 0 ? (kbps / totalKbps) * 100 : (100 / links.length);
      let color;
      if (kbps === 0)        color = "#5a5f6d";
      else if (kbps < 100)  color = "#da3633";
      else if (kbps <= 500) color = "#d29922";
      else                   color = "#2ea043";
      return { pct, color };
    });
  } else if (conn.stats?.available) {
    const kbps = conn.stats.bitrate_kbps || 0;
    let color;
    if (kbps === 0)        color = "#5a5f6d";
    else if (kbps < 100)  color = "#da3633";
    else if (kbps <= 500) color = "#d29922";
    else                   color = "#2ea043";
    segments = [{ pct: 100, color }];
  } else {
    return null;
  }

  return (
    <div
      onClick={onClick}
      title={isExpanded ? "Collapse per-link stats" : "Expand per-link stats"}
      style={{
        display: "flex", width: 48, height: 5, borderRadius: 2,
        overflow: "hidden", flexShrink: 0, cursor: "pointer",
        opacity: isExpanded ? 0.65 : 1,
        transition: "opacity 0.15s",
      }}
    >
      {segments.map((seg, i) => (
        <div key={i} style={{ flex: seg.pct, height: "100%", background: seg.color, minWidth: 2 }} />
      ))}
    </div>
  );
}

// --- Inline per-link Mbps bars shown under active relay connections ---
function RelayLinkBars({ conn }) {
  if (conn.status !== "connected") return null;

  const hasPerLink = conn.per_link?.available && Array.isArray(conn.per_link?.links) && conn.per_link.links.length > 0;

  const boundedStyle = {
    marginTop: 5,
    paddingLeft: 11,
    maxHeight: 96,
    overflowY: "auto",
    padding: "4px 6px",
    border: "1px solid #2a2d3550",
    borderRadius: 3,
    background: "rgba(255,255,255,0.015)",
    transition: "max-height 0.25s ease",
  };

  if (hasPerLink) {
    const maxKbps = Math.max(...conn.per_link.links.map(l => l.bitrate_kbps || 0), 1000) * 1.25;
    return (
      <div style={boundedStyle}>
        {conn.per_link.links.map((link, i) => (
          <BitrateBar
            key={link.carrier || i}
            label={link.carrier || ("Link " + (i + 1))}
            value={link.bitrate_kbps || 0}
            max={maxKbps}
            color="#2d7aed"
          />
        ))}
      </div>
    );
  }

  if (conn.stats?.available && conn.stats.bitrate_kbps > 0) {
    const maxKbps = Math.max(conn.stats.bitrate_kbps * 1.5, 1000);
    return (
      <div style={boundedStyle}>
        <BitrateBar
          label="Total"
          value={conn.stats.bitrate_kbps}
          max={maxKbps}
          color="#2d7aed"
        />
      </div>
    );
  }

  return null;
}

// --- Single connection row ---
function ConnectionRow({ conn, sendAction, isCompact }) {
  const [showDetails, setShowDetails] = useState(false);
  const [showLinks, setShowLinks] = useState(false);
  const statusColor = connStatusColor(conn.status);
  const isConnected  = conn.status === "connected";
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
  const actionColor = isConnected ? "var(--theme-text-muted, #5a5f6d)"
    : isConnecting ? "#d29922"
    : "#5ba3f5";

  const showStatus = isConnected || isConnecting || conn.status === "error";
  const statusText = isConnected ? "Active" : isConnecting ? "Connecting\u2026" : conn.error_msg || "Error";
  const statusTextColor = isConnected ? "#2ea043" : isConnecting ? "#d29922" : "#da3633";
  const inlineStat = getRelayInlineStat(conn);

  return (
    <div style={{ borderBottom: "1px solid var(--theme-border, #13151a)", padding: "7px 0" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 5 }}>
        <StatusDot color={statusColor} pulse={isConnected || isConnecting} />
        <div style={{ flex: 1, overflow: "hidden", minWidth: 0, display: "flex", alignItems: "baseline", gap: 5 }}>
          {showStatus && (
            <span style={{
              fontSize: 8, fontWeight: 500, flexShrink: 0,
              color: statusTextColor,
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            }}>
              {statusText}
            </span>
          )}
          <span style={{
            fontSize: isCompact ? 9 : 10, fontWeight: 600,
            color: "var(--theme-text, #e0e2e8)",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
          }}>{conn.name}</span>
          {inlineStat && (
            <span style={{
              fontSize: 8, color: "var(--theme-text-muted, #8b8f98)", flexShrink: 0, whiteSpace: "nowrap",
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            }}>
              {inlineStat.mbps} Mbps{inlineStat.rttMs > 0 ? " \u00b7 " + inlineStat.rttMs + "ms" : ""}
            </span>
          )}
        </div>
        <MiniHealthBar conn={conn} onClick={() => setShowLinks(s => !s)} isExpanded={showLinks} />
        <ConnectionTypeBadge type={conn.type} />
        <button onClick={handleAction} style={{
          height: 20, padding: "0 8px", borderRadius: 3,
          border: "1px solid var(--theme-border, #2a2d35)",
          background: "var(--theme-panel, #20232b)",
          color: actionColor,
          fontSize: 9, fontWeight: 600, cursor: "pointer", flexShrink: 0,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>{actionLabel}</button>
        <button
          onClick={() => setShowDetails(!showDetails)}
          style={{
            border: "none", background: "none",
            color: "var(--theme-text-muted, #4a4f5c)", fontSize: 9, cursor: "pointer",
            padding: "0 2px", flexShrink: 0,
            display: "flex", alignItems: "center",
          }}
        >
          <span style={{ fontSize: 8, lineHeight: 1 }}>{showDetails ? "\u25b2" : "\u25bc"}</span>
        </button>
      </div>

      {isConnected && (
        <div style={{
          maxHeight: showLinks ? 200 : 0,
          overflow: "hidden",
          transition: "max-height 0.2s ease",
        }}>
          <RelayLinkBars conn={conn} />
        </div>
      )}

      {isConnecting && conn.type === "managed" && (
        <ManagedProvisionProgress step={conn.provision_step} />
      )}

      {showDetails && (
        <ConnectionExpandedDetail conn={conn} sendAction={sendAction} onRemove={handleRemove} />
      )}
    </div>
  );
}

// --- Add Connection inline form (expands in place, no overlay) ---
function AddConnectionForm({ onClose, sendAction, authAuthenticated, authPlanLabel, authPending, authLogin, authEntitlement, handleAuthLogin, handleAuthOpenBrowser }) {
  const [name, setName] = useState("");
  const [type, setType] = useState("byor");
  const [host, setHost] = useState("");
  const [port, setPort] = useState("5000");
  const [streamId, setStreamId] = useState("");
  const [region, setRegion] = useState("us-east");
  const [error, setError] = useState(null);

  const maxConns = authEntitlement?.max_concurrent_conns || 0;
  const activeConns = authEntitlement?.active_managed_conns || 0;
  const isManagedLimitReached = type === "managed" && authAuthenticated && maxConns > 0 && activeConns >= maxConns;

  const handleAdd = () => {
    const trimName = name.trim();
    if (!trimName) { setError("Name is required"); return; }

    if (type === "byor") {
      const trimHost = host.trim();
      if (!trimHost) { setError("Relay host is required"); return; }
      const parsedPort = parseInt(port, 10);
      if (!Number.isFinite(parsedPort) || parsedPort < 1 || parsedPort > 65535) {
        setError("Invalid port (1\u201365535)"); return;
      }
      sendAction({
        type: "connection_add", requestId: genRequestId(),
        name: trimName, conn_type: "byor",
        relay_host: trimHost, relay_port: parsedPort, stream_id: streamId.trim(),
      });
    } else {
      if (!authAuthenticated) { setError("Login required for managed relays"); return; }
      if (isManagedLimitReached) { setError("Connection limit reached for your plan"); return; }
      sendAction({
        type: "connection_add", requestId: genRequestId(),
        name: trimName, conn_type: "managed", managed_region: region,
      });
    }
    onClose();
  };

  const inputStyle = {
    width: "100%", height: 23, borderRadius: 3,
    border: "1px solid var(--theme-border, #2a2d35)",
    background: "var(--theme-bg, #0c0e13)",
    color: "var(--theme-text, #e0e2e8)",
    fontSize: 10, padding: "0 8px",
    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
    boxSizing: "border-box",
  };

  const labelStyle = {
    fontSize: 9, color: "var(--theme-text-muted, #8b8f98)",
    textTransform: "uppercase", letterSpacing: "0.04em", marginBottom: 3,
    fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
  };

  const addDisabled = type === "managed" && (!authAuthenticated || isManagedLimitReached);

  return (
    <div style={{
      marginTop: 6,
      background: "var(--theme-surface, #13151a)",
      border: "1px solid var(--theme-border, #2a2d35)",
      borderRadius: 4,
      padding: "10px 10px 12px",
    }}>
      <div style={{ display: "flex", alignItems: "center", marginBottom: 10 }}>
        <span style={{
          flex: 1, fontSize: 10, fontWeight: 700,
          color: "var(--theme-text, #e0e2e8)",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>New Connection</span>
        <button onClick={onClose} style={{
          border: "none", background: "none",
          color: "var(--theme-text-muted, #8b8f98)",
          fontSize: 13, cursor: "pointer", padding: "0 2px", lineHeight: 1,
        }}>{"\u2715"}</button>
      </div>

      <div style={{ marginBottom: 8 }}>
        <div style={labelStyle}>Name</div>
        <input
          value={name}
          onChange={e => setName(e.target.value)}
          placeholder="e.g. Main Cam \u2192 My VPS"
          style={inputStyle}
          autoFocus
        />
      </div>

      <div style={{ marginBottom: 8 }}>
        <div style={labelStyle}>Type</div>
        <div style={{ display: "flex", gap: 5 }}>
          <button onClick={() => setType("byor")} style={{
            flex: 1, height: 26, borderRadius: 3,
            border: type === "byor" ? "1px solid #2d7aed80" : "1px solid var(--theme-border, #2a2d35)",
            background: type === "byor" ? "#1a3a5a" : "var(--theme-panel, #20232b)",
            color: type === "byor" ? "#5ba3f5" : "var(--theme-text-muted, #8b8f98)",
            fontSize: 9, fontWeight: 700, cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>BYOR</button>
          <button onClick={() => setType("managed")} style={{
            flex: 1, height: 26, borderRadius: 3,
            border: type === "managed" ? "1px solid #2ea04380" : "1px solid var(--theme-border, #2a2d35)",
            background: type === "managed" ? "#1a3a2a" : "var(--theme-panel, #20232b)",
            color: type === "managed" ? "#4ade80" : "var(--theme-text-muted, #8b8f98)",
            fontSize: 9, fontWeight: 700, cursor: "pointer",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>Managed</button>
        </div>
      </div>

      {type === "byor" && (
        <div>
          <div style={{ marginBottom: 7 }}>
            <div style={labelStyle}>Relay Host</div>
            <input
              value={host}
              onChange={e => setHost(e.target.value)}
              placeholder="relay.example.com"
              style={inputStyle}
            />
          </div>
          <div style={{ display: "grid", gridTemplateColumns: "80px minmax(0,1fr)", gap: 7, marginBottom: 7 }}>
            <div>
              <div style={labelStyle}>Port</div>
              <input
                value={port}
                onChange={e => setPort(e.target.value.replace(/[^\d]/g, "").slice(0, 5))}
                inputMode="numeric"
                placeholder="5000"
                style={inputStyle}
              />
            </div>
            <div>
              <div style={labelStyle}>Stream ID <span style={{ textTransform: "none", letterSpacing: 0, opacity: 0.55 }}>(optional)</span></div>
              <input
                value={streamId}
                onChange={e => setStreamId(e.target.value)}
                placeholder="live/stream"
                style={inputStyle}
              />
            </div>
          </div>
          <div style={{
            fontSize: 9, color: "var(--theme-text-muted, #4a4f5c)", marginBottom: 8,
            padding: "4px 7px", background: "var(--theme-panel, #20232b)", borderRadius: 3,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>
            Sensitive fields are stored encrypted locally (DPAPI)
          </div>
        </div>
      )}

      {type === "managed" && !authAuthenticated && (
        <div style={{
          marginBottom: 8, padding: "8px 8px",
          background: "var(--theme-panel, #20232b)", borderRadius: 3,
          border: "1px solid var(--theme-border, #2a2d35)",
        }}>
          <div style={{
            fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontWeight: 600, marginBottom: 4,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>Login required</div>
          <div style={{
            fontSize: 9, color: "var(--theme-text-muted, #8b8f98)", marginBottom: 7, lineHeight: 1.5,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>
            Sign in to provision a Telemy Managed Relay.
          </div>
          <button
            onClick={handleAuthLogin}
            disabled={authPending}
            style={{
              width: "100%", padding: "6px 0",
              border: "1px solid var(--theme-border, #2a2d35)",
              borderRadius: 3, background: "var(--theme-surface, #13151a)",
              cursor: authPending ? "not-allowed" : "pointer",
              color: "#5ba3f5", fontSize: 10, fontWeight: 600,
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              opacity: authPending ? 0.7 : 1,
            }}
          >
            {authPending ? "Waiting for browser\u2026" : "Sign In"}
          </button>
          {authPending && authLogin && authLogin.authorize_url && (
            <button
              onClick={handleAuthOpenBrowser}
              style={{
                width: "100%", marginTop: 5, padding: "4px 0",
                border: "1px solid var(--theme-border, #2a2d35)",
                borderRadius: 3, background: "var(--theme-panel, #20232b)",
                cursor: "pointer", color: "var(--theme-text-muted, #8b8f98)", fontSize: 9,
                fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              }}
            >
              Open Browser
            </button>
          )}
        </div>
      )}

      {type === "managed" && authAuthenticated && isManagedLimitReached && (
        <div style={{
          marginBottom: 8, padding: "6px 8px",
          background: "rgba(210,153,34,0.08)", borderRadius: 3,
          border: "1px solid #d2992240",
        }}>
          <div style={{
            fontSize: 9, color: "#d29922",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>
            Connection limit reached for {authPlanLabel} plan ({maxConns} max). Upgrade to add more.
          </div>
        </div>
      )}

      {type === "managed" && authAuthenticated && !isManagedLimitReached && (
        <div style={{ marginBottom: 8 }}>
          <div style={labelStyle}>Region</div>
          <select
            value={region}
            onChange={e => setRegion(e.target.value)}
            style={{ ...inputStyle, height: 25, cursor: "pointer" }}
          >
            {MANAGED_REGIONS.map(r => <option key={r.id} value={r.id}>{r.label}</option>)}
          </select>
        </div>
      )}

      {error && (
        <div style={{
          fontSize: 9, color: "#da3633", marginBottom: 7, padding: "4px 7px",
          background: "rgba(218,54,51,0.08)", borderRadius: 3,
          border: "1px solid #da363340",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>{error}</div>
      )}

      <div style={{ display: "flex", gap: 5 }}>
        <button onClick={onClose} style={{
          height: 26, padding: "0 12px", borderRadius: 3,
          border: "1px solid var(--theme-border, #2a2d35)",
          background: "var(--theme-panel, #20232b)",
          color: "var(--theme-text-muted, #8b8f98)", fontSize: 10, cursor: "pointer",
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>Cancel</button>
        <button onClick={handleAdd} disabled={addDisabled} style={{
          flex: 1, height: 26, borderRadius: 3, border: "none",
          background: addDisabled ? "var(--theme-panel, #20232b)" : "#2d7aed",
          color: addDisabled ? "var(--theme-text-muted, #5a5f6d)" : "#fff",
          fontSize: 10, fontWeight: 600,
          cursor: addDisabled ? "not-allowed" : "pointer",
          opacity: addDisabled ? 0.5 : 1,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>Add Relay</button>
      </div>
    </div>
  );
}

// --- Main section component (exported — used in AegisDock) ---
export function ConnectionListSection({
  relayConnections,
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
  isCompact,
}) {
  const [showAddModal, setShowAddModal] = useState(false);
  const connections = Array.isArray(relayConnections) ? relayConnections : [];

  return (
    <div>
      <div style={{
        display: "flex", alignItems: "center", justifyContent: "space-between",
        marginBottom: 6, padding: "5px 8px",
        background: "var(--theme-surface, #13151a)", borderRadius: 4,
        border: "1px solid var(--theme-border, #2a2d35)",
      }}>
        {authAuthenticated ? (
          <span style={{
            fontSize: 9, color: "var(--theme-text, #e0e2e8)", flex: 1,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
            marginRight: 6,
          }}>
            {authDisplayName}
            <span style={{ color: "var(--theme-text-muted, #5a5f6d)", marginLeft: 5 }}>
              {"\u00b7"} {authPlanLabel}
            </span>
          </span>
        ) : (
          <span style={{
            fontSize: 9, color: "var(--theme-text-muted, #5a5f6d)", flex: 1,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>
            Sign in for Telemy Relays
          </span>
        )}
        {authAuthenticated ? (
          <button onClick={handleAuthLogout} style={{
            border: "none", background: "none",
            color: "var(--theme-text-muted, #4a4f5c)", fontSize: 9,
            cursor: "pointer", padding: 0, flexShrink: 0,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          }}>Sign out</button>
        ) : (
          <button
            onClick={handleAuthLogin}
            disabled={authPending}
            style={{
              height: 20, padding: "0 8px", borderRadius: 3,
              border: "1px solid #2d7aed40", background: "#1a3a5a",
              color: "#5ba3f5", fontSize: 9, fontWeight: 600, flexShrink: 0,
              cursor: authPending ? "not-allowed" : "pointer",
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              opacity: authPending ? 0.7 : 1,
            }}
          >
            {authPending ? "Waiting\u2026" : "Sign In"}
          </button>
        )}
      </div>

      {connections.length === 0 && (
        <div style={{
          textAlign: "center", padding: "14px 0",
          color: "var(--theme-text-muted, #4a4f5c)", fontSize: 10,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        }}>
          No relay connections. Press + to add one.
        </div>
      )}

      {connections.map(conn => (
        <ConnectionRow key={conn.id} conn={conn} sendAction={sendAction} isCompact={isCompact} />
      ))}

      {!showAddModal && (
        <button
          onClick={() => setShowAddModal(true)}
          style={{
            width: "100%", marginTop: connections.length > 0 ? 6 : 2,
            height: 26, borderRadius: 4,
            border: "1px dashed var(--theme-border, #2a2d35)",
            background: "transparent",
            color: "var(--theme-text-muted, #4a4f5c)",
            fontSize: 11, cursor: "pointer",
            display: "flex", alignItems: "center", justifyContent: "center", gap: 4,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            transition: "border-color 0.15s, color 0.15s",
          }}
          onMouseEnter={e => { e.currentTarget.style.borderColor = "#2d7aed80"; e.currentTarget.style.color = "#5ba3f5"; }}
          onMouseLeave={e => { e.currentTarget.style.borderColor = "var(--theme-border, #2a2d35)"; e.currentTarget.style.color = "var(--theme-text-muted, #4a4f5c)"; }}
        >
          <span style={{ fontSize: 14, lineHeight: 1, marginTop: -1 }}>+</span>
          <span style={{ fontSize: 9, letterSpacing: "0.04em" }}>Add Relay</span>
        </button>
      )}

      {showAddModal && (
        <AddConnectionForm
          onClose={() => setShowAddModal(false)}
          sendAction={sendAction}
          authAuthenticated={authAuthenticated}
          authPlanLabel={authPlanLabel}
          authPending={authPending}
          authLogin={authLogin}
          authEntitlement={authEntitlement}
          handleAuthLogin={handleAuthLogin}
          handleAuthOpenBrowser={handleAuthOpenBrowser}
        />
      )}
    </div>
  );
}
