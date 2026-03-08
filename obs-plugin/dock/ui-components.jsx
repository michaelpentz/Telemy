import { useState } from "react";
import { ENGINE_STATES } from "./constants.js";

// =============================================================================
// UI COMPONENTS
// =============================================================================

// --- Collapsible Section ---
export function Section({ title, icon, badge, badgeColor, defaultOpen = false, compact = false, children }) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <div style={{
      borderBottom: "1px solid var(--theme-border, #1a1d23)",
      background: open ? "rgba(128,128,128,0.04)" : "transparent",
      transition: "background 0.2s ease",
    }}>
      <button
        onClick={() => setOpen(!open)}
        style={{
          width: "100%", display: "flex", alignItems: "center", gap: 8,
          padding: compact ? "9px 10px" : "10px 12px", border: "none", background: "none",
          color: "var(--theme-text-muted, #c8ccd4)", cursor: "pointer", fontSize: compact ? 10 : 11,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
          fontWeight: 600, letterSpacing: "0.08em", textTransform: "uppercase",
          transition: "color 0.15s ease",
        }}
        onMouseEnter={e => e.currentTarget.style.color = "var(--theme-text, #fff)"}
        onMouseLeave={e => e.currentTarget.style.color = "var(--theme-text-muted, #c8ccd4)"}
      >
        <span style={{ fontSize: 13, opacity: 0.6, lineHeight: 1 }}>{icon}</span>
        <span style={{ flex: 1, textAlign: "left" }}>{title}</span>
        {badge != null && (
          <span style={{
            background: badgeColor || "#2d7aed",
            color: "var(--theme-bg, #fff)", fontSize: compact ? 7 : 8, fontWeight: 700,
            padding: compact ? "1px 4px" : "2px 6px", borderRadius: 3, letterSpacing: "0.04em",
            fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
            maxWidth: compact ? 64 : 92,
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap",
          }}>{badge}</span>
        )}
        <svg width="10" height="10" viewBox="0 0 10 10"
          style={{
            transform: open ? "rotate(0deg)" : "rotate(-90deg)",
            transition: "transform 0.2s ease", opacity: 0.4,
          }}>
          <path d="M2 3.5L5 6.5L8 3.5" stroke="currentColor" strokeWidth="1.5"
            fill="none" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </button>
      <div style={{
        maxHeight: open ? 800 : 0,
        overflow: "hidden",
        transition: "max-height 0.3s cubic-bezier(0.4,0,0.2,1)",
        opacity: open ? 1 : 0,
      }}>
        <div style={{ padding: compact ? "2px 10px 10px" : "2px 12px 12px" }}>
          {children}
        </div>
      </div>
    </div>
  );
}

// --- Status Dot ---
export function StatusDot({ color, pulse }) {
  return (
    <span style={{ position: "relative", display: "inline-flex", alignItems: "center", justifyContent: "center" }}>
      {pulse && (
        <span style={{
          position: "absolute", width: 10, height: 10, borderRadius: "50%",
          background: color, opacity: 0.3,
          animation: "pulse 2s ease-in-out infinite",
        }} />
      )}
      <span style={{
        width: 6, height: 6, borderRadius: "50%", background: color,
        boxShadow: `0 0 6px ${color}40`,
      }} />
    </span>
  );
}

// --- Bitrate Bar ---
export function BitrateBar({ value, max, color, label }) {
  const pct = max > 0 ? Math.min((value / max) * 100, 100) : 0;
  return (
    <div style={{ marginBottom: 6 }}>
      <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 3 }}>
        <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)" }}>{label}</span>
        <span style={{ fontSize: 10, color: "var(--theme-text, #e0e2e8)", fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)", fontWeight: 600 }}>
          {value >= 1000 ? (value / 1000).toFixed(1) + " Mbps" : Math.round(value) + " kbps"}
        </span>
      </div>
      <div style={{ height: 4, background: "var(--theme-surface, #1a1d23)", borderRadius: 2, overflow: "hidden" }}>
        <div style={{
          height: "100%", width: `${pct}%`,
          background: `linear-gradient(90deg, ${color}, ${color}cc)`,
          borderRadius: 2, transition: "width 0.6s cubic-bezier(0.4,0,0.2,1)",
          boxShadow: `0 0 8px ${color}30`,
        }} />
      </div>
    </div>
  );
}

// --- Stat Pill (compact metric display) ---
export function StatPill({ label, value, color }) {
  return (
    <div style={{ flex: 1, textAlign: "center" }}>
      <div style={{ fontSize: 8, color: "var(--theme-text-muted, #8b8f98)",
        textTransform: "uppercase", letterSpacing: "0.4px" }}>{label}</div>
      <div style={{ fontSize: 11, fontWeight: 600, color: color || "var(--theme-text, #e0e2e8)" }}>
        {value}
      </div>
    </div>
  );
}

// --- Toggle Row (fully controlled — state comes from props) ---
export function ToggleRow({ label, value, color, dimmed, onChange }) {
  const isOn = !!value;
  const isDimmed = dimmed || value === null;
  return (
    <div style={{
      display: "flex", alignItems: "center", justifyContent: "space-between",
      padding: "6px 0", borderBottom: "1px solid var(--theme-border, #13151a)",
      opacity: isDimmed ? 0.45 : 1,
    }}>
      <span style={{
        fontSize: 10, color: isOn ? "var(--theme-text, #c8ccd4)" : "var(--theme-text-muted, #5a5f6d)",
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
      }}>{label}</span>
      <button onClick={() => { if (!isDimmed && onChange) onChange(!isOn); }} style={{
        width: 32, height: 16, borderRadius: 8, border: "none",
        cursor: isDimmed ? "not-allowed" : "pointer",
        background: isOn ? (color || "var(--theme-accent, #2d7aed)") : "var(--theme-border, #2a2d35)",
        position: "relative", transition: "background 0.2s ease",
        flexShrink: 0,
      }}>
        <div style={{
          width: 12, height: 12, borderRadius: "50%", background: "var(--theme-text, #fff)",
          position: "absolute", top: 2,
          left: isOn ? 18 : 2,
          transition: "left 0.2s ease",
          boxShadow: "0 1px 3px rgba(0,0,0,0.3)",
        }} />
      </button>
    </div>
  );
}

// --- Connection Card ---
// Uses normalized bridge fields: { name, type, signal, bitrate, status }
export function ConnectionCard({ name, type, signal, bitrate, status, compact = false }) {
  const statusColors = { connected: "#2ea043", degraded: "#d29922", disconnected: "#da3633" };
  const col = statusColors[status] || statusColors.disconnected;
  const bars = [1, 2, 3, 4];
  return (
    <div style={{
      background: "var(--theme-surface, #13151a)", borderRadius: 4, padding: "8px 10px",
      border: `1px solid ${status === "connected" ? "var(--theme-accent, #1a3a1a)" : "var(--theme-border, #2a2d35)"}`,
      marginBottom: 4, transition: "border-color 0.2s ease",
    }}>
      <div style={{ display: "flex", alignItems: "center", gap: compact ? 4 : 6, marginBottom: 4 }}>
        <StatusDot color={col} pulse={status === "connected"} />
        <span style={{
          fontSize: compact ? 10 : 11, color: "var(--theme-text, #e0e2e8)", fontWeight: 600, flex: 1,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
          overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
        }}>{name}</span>
        <span style={{
          fontSize: compact ? 8 : 9, color: "var(--theme-text-muted, #6b7080)", fontWeight: 500,
          background: "var(--theme-surface, #1a1d23)", padding: compact ? "1px 4px" : "1px 5px", borderRadius: 2,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)", flexShrink: 0,
        }}>{type}</span>
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
        <div style={{ display: "flex", gap: 1.5, alignItems: "flex-end" }}>
          {bars.map(i => (
            <div key={i} style={{
              width: 3, height: 3 + i * 3, borderRadius: 1,
              background: i <= signal ? col : "var(--theme-border, #2a2d35)",
              transition: "background 0.3s ease",
              boxShadow: i <= signal ? `0 0 3px ${col}30` : "none",
            }} />
          ))}
        </div>
        <span style={{ fontSize: 10, color: "var(--theme-text-muted, #8b8f98)", fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)" }}>
          {bitrate > 0 ? `${(bitrate / 1000).toFixed(1)} Mbps` : "\u2014"}
        </span>
      </div>
    </div>
  );
}

// --- Engine State Chips (3x2 grid, canonical states from STATE_MACHINE_v1.md) ---
export function EngineStateChips({ activeState, compact = false }) {
  return (
    <div style={{
      display: "grid", gridTemplateColumns: compact ? "1fr 1fr" : "1fr 1fr 1fr",
      gap: 3, marginBottom: 8,
    }}>
      {ENGINE_STATES.map((es) => {
        const isActive = activeState === es.id;
        return (
          <div key={es.id} style={{
            height: compact ? 20 : 22, borderRadius: 3, display: "flex",
            alignItems: "center", justifyContent: "center",
            fontSize: compact ? 6 : 7, fontWeight: 700, letterSpacing: "0.04em",
            fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
            background: isActive ? es.bgActive : "var(--theme-surface, #13151a)",
            border: `1px solid ${isActive ? es.borderActive : "var(--theme-border, #2a2d35)"}`,
            color: isActive ? es.color : "var(--theme-text-muted, #5a5f6d)",
            transition: "all 0.25s ease",
            boxShadow: isActive ? `0 0 8px ${es.color}15` : "none",
          }}>
            {es.short}
          </div>
        );
      })}
    </div>
  );
}
