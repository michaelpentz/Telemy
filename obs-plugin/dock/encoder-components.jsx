import { useState, useEffect, useRef, useCallback, useMemo } from "react";
import { OUTPUT_HEALTH_COLORS, getOutputHealthColor, OUTPUT_CONFIG_STORAGE_KEY } from "./constants.js";
import { useAnimatedValue } from "./hooks.js";

// --- Output Bar ---
// Renders a single encoder output as a labelled bitrate bar with health color.
export function OutputBar({ name, bitrateKbps, fps, dropPct, active, maxBitrate, compact = false }) {
  const healthColor = getOutputHealthColor(bitrateKbps, maxBitrate);
  const animBitrate = useAnimatedValue(active ? (bitrateKbps || 0) : 0, 600);
  const pct = maxBitrate > 0 ? Math.min((animBitrate / maxBitrate) * 100, 100) : 0;
  const inactive = !active;

  return (
    <div style={{ marginBottom: compact ? 6 : 8, opacity: inactive ? 0.4 : 1, transition: "opacity 0.3s ease" }}>
      <div style={{
        display: "flex", justifyContent: "space-between", alignItems: "baseline",
        marginBottom: 3, fontSize: compact ? 9 : 10,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
      }}>
        <span style={{
          color: "var(--theme-text, #e0e2e8)", fontWeight: 600,
          overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", flex: 1, marginRight: 8,
        }}>
          {name || "Output"}
        </span>
        <span style={{ color: "var(--theme-text-muted, #8b8f98)", flexShrink: 0, whiteSpace: "nowrap" }}>
          {inactive ? "\u2014" : (
            <>
              {bitrateKbps != null ? `${(bitrateKbps / 1000).toFixed(1)} Mbps` : "\u2014"}
              {"  "}
              {fps != null ? `${Math.round(fps)}fps` : ""}
              {"  "}
              {dropPct != null ? `${dropPct.toFixed(2)}%` : ""}
            </>
          )}
        </span>
      </div>
      <div style={{
        height: compact ? 3 : 4, background: "var(--theme-border, #2a2d35)",
        borderRadius: 2, overflow: "hidden",
      }}>
        <div style={{
          height: "100%", width: `${pct}%`,
          background: healthColor,
          borderRadius: 2,
          transition: "width 0.6s cubic-bezier(0.4,0,0.2,1), background 0.4s ease",
          boxShadow: inactive ? "none" : `0 0 4px ${healthColor}30`,
        }} />
      </div>
    </div>
  );
}

// --- Encoder Group Header ---
// Divider row labelling a named encoder pool with aggregate bitrate + lag.
export function EncoderGroupHeader({ name, resolution, totalBitrateKbps, avgLagMs, compact = false }) {
  if (name === "Ungrouped") return null;
  return (
    <div style={{ marginTop: 8, marginBottom: 6 }}>
      <div style={{
        display: "flex", alignItems: "center", gap: 6,
        fontSize: compact ? 8 : 9,
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        color: "var(--theme-text-muted, #8b8f98)",
        letterSpacing: "0.05em", fontWeight: 700,
      }}>
        <div style={{ flex: 1, height: 1, background: "var(--theme-border, #2a2d35)" }} />
        <span>{name.toUpperCase()}</span>
        {resolution && (
          <span style={{
            fontSize: compact ? 7 : 8, padding: "1px 4px", borderRadius: 2,
            background: "var(--theme-surface, #13151a)", border: "1px solid var(--theme-border, #2a2d35)",
          }}>
            {resolution}
          </span>
        )}
        <div style={{ flex: 1, height: 1, background: "var(--theme-border, #2a2d35)" }} />
      </div>
      <div style={{
        fontSize: compact ? 8 : 9, color: "var(--theme-text-muted, #6b7080)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        textAlign: "center", marginTop: 3,
      }}>
        Pool {totalBitrateKbps != null ? `${(totalBitrateKbps / 1000).toFixed(1)} Mbps` : "\u2014"}
        {"  \u2022  "}
        Lag {avgLagMs != null ? `${avgLagMs.toFixed(1)}ms` : "\u2014"}
      </div>
    </div>
  );
}

// --- Hidden Outputs Toggle ---
// Collapsible list of outputs that are hidden from the main section.
export function HiddenOutputsToggle({ items, compact = false }) {
  const [expanded, setExpanded] = useState(false);
  if (!items || items.length === 0) return null;
  return (
    <div style={{ marginTop: 6 }}>
      <div
        onClick={() => setExpanded(v => !v)}
        style={{
          display: "flex", alignItems: "center", gap: 6, cursor: "pointer",
          fontSize: compact ? 8 : 9,
          fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
          color: "var(--theme-text-muted, #6b7080)",
        }}
      >
        <span>Hidden ({items.length})</span>
        <span style={{
          fontSize: compact ? 7 : 8, color: "var(--theme-accent, #5ba3f5)",
          textDecoration: "underline", textUnderlineOffset: 2,
        }}>
          {expanded ? "Hide" : "Show"}
        </span>
      </div>
      {expanded && (
        <div style={{ marginTop: 4, opacity: 0.5 }}>
          {items.map((item, idx) => (
            <div key={item.id || idx} style={{
              fontSize: compact ? 8 : 9, color: "var(--theme-text-muted, #6b7080)",
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              marginBottom: 2,
            }}>
              {item.name || item.platform || `Output ${idx + 1}`}
              {item.active ? " (active)" : ""}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

// --- Output Config Row ---
// Per-output rename / group assignment / visibility toggle.
export function OutputConfigRow({ output, config, onUpdate, compact = false }) {
  const [editing, setEditing] = useState(null); // "name" | "group" | null
  const inputRef = useRef(null);
  const displayName = config?.displayName || output.name || output.platform || output.id;
  const groupName = config?.group || "";
  const isHidden = config?.hidden || false;

  useEffect(() => {
    if (editing && inputRef.current) inputRef.current.focus();
  }, [editing]);

  const commitEdit = (field, value) => {
    setEditing(null);
    const trimmed = value.trim();
    if (field === "name" && trimmed && trimmed !== displayName) {
      onUpdate({ ...config, displayName: trimmed });
    } else if (field === "group") {
      onUpdate({ ...config, group: trimmed || null });
    }
  };

  const rowFs = compact ? 9 : 10;
  return (
    <div style={{
      display: "flex", alignItems: "center", gap: 6,
      padding: "5px 0", borderBottom: "1px solid var(--theme-border, #13151a)",
      opacity: isHidden ? 0.4 : 1, transition: "opacity 0.2s ease",
    }}>
      {/* Visibility toggle */}
      <button
        onClick={() => onUpdate({ ...config, hidden: !isHidden })}
        title={isHidden ? "Show output" : "Hide output"}
        style={{
          width: 16, height: 16, flexShrink: 0, cursor: "pointer",
          background: "none", border: "none", padding: 0,
          fontSize: 11, lineHeight: 1, color: isHidden ? "var(--theme-text-muted, #3a3d45)" : "#2ea043",
        }}
      >
        {isHidden ? "\u25CB" : "\u25C9"}
      </button>

      {/* Name — click to edit */}
      <div style={{ flex: 1, minWidth: 0 }}>
        {editing === "name" ? (
          <input
            ref={inputRef}
            defaultValue={displayName}
            onBlur={(e) => commitEdit("name", e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter") commitEdit("name", e.target.value);
              if (e.key === "Escape") setEditing(null);
            }}
            style={{
              width: "100%", fontSize: rowFs, padding: "1px 4px",
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              background: "var(--theme-surface, #13151a)", color: "var(--theme-text, #e0e2e8)",
              border: "1px solid var(--theme-accent, #5ba3f5)", borderRadius: 2, outline: "none",
            }}
          />
        ) : (
          <span
            onClick={() => setEditing("name")}
            title="Click to rename"
            style={{
              fontSize: rowFs, cursor: "pointer",
              fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
              color: "var(--theme-text, #e0e2e8)", fontWeight: 500,
              overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", display: "block",
            }}
          >
            {displayName}
          </span>
        )}
      </div>

      {/* Group — click to edit */}
      {editing === "group" ? (
        <input
          ref={inputRef}
          defaultValue={groupName}
          placeholder="Group"
          onBlur={(e) => commitEdit("group", e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") commitEdit("group", e.target.value);
            if (e.key === "Escape") setEditing(null);
          }}
          style={{
            width: 60, fontSize: compact ? 8 : 9, padding: "1px 4px",
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            background: "var(--theme-surface, #13151a)", color: "var(--theme-text, #e0e2e8)",
            border: "1px solid var(--theme-accent, #5ba3f5)", borderRadius: 2, outline: "none",
          }}
        />
      ) : (
        <span
          onClick={() => setEditing("group")}
          title="Click to set group"
          style={{
            fontSize: compact ? 8 : 9, cursor: "pointer", flexShrink: 0,
            fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
            color: groupName ? "var(--theme-text-muted, #8b8f98)" : "var(--theme-text-muted, #3a3d45)",
            background: "var(--theme-surface, #13151a)", padding: "1px 5px", borderRadius: 2,
            border: "1px solid var(--theme-border, #2a2d35)",
          }}
        >
          {groupName || "\u2014"}
        </span>
      )}
    </div>
  );
}

// --- Output Config Panel ---
// Renders inside a Section. Shows all known outputs with rename/group/hide controls.
// Config is persisted to localStorage and sent to native via set_output_config action.
export function OutputConfigPanel({ encoderOutputs, sendAction, compact = false }) {
  const [outputConfig, setOutputConfig] = useState(() => {
    try {
      const raw = localStorage.getItem(OUTPUT_CONFIG_STORAGE_KEY);
      return raw ? JSON.parse(raw) : {};
    } catch (_) { return {}; }
  });

  // Merge all outputs (visible + hidden) into a single list for config
  const allOutputs = useMemo(() => {
    const items = [];
    if (encoderOutputs.groups) {
      for (const group of encoderOutputs.groups) {
        if (group.items) items.push(...group.items);
      }
    }
    if (encoderOutputs.hidden) items.push(...encoderOutputs.hidden);
    return items;
  }, [encoderOutputs]);

  const handleUpdate = useCallback((outputId, newConfig) => {
    setOutputConfig(prev => {
      const next = { ...prev, [outputId]: newConfig };
      try { localStorage.setItem(OUTPUT_CONFIG_STORAGE_KEY, JSON.stringify(next)); } catch (_) {}
      // Forward to native for persistence
      sendAction({ type: "set_output_config", outputId, config: newConfig });
      return next;
    });
  }, [sendAction]);

  if (allOutputs.length === 0) {
    return (
      <div style={{
        fontSize: compact ? 9 : 10, color: "var(--theme-text-muted, #3a3d45)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        padding: "8px 0", textAlign: "center",
      }}>
        No outputs detected
      </div>
    );
  }

  return (
    <div>
      <div style={{
        fontSize: compact ? 7 : 8, color: "var(--theme-text-muted, #5a5f6d)",
        fontFamily: "var(--theme-font-family, 'Segoe UI', system-ui, sans-serif)",
        padding: "2px 0 6px", letterSpacing: "0.04em",
      }}>
        Click name or group to edit. Toggle visibility with the dot.
      </div>
      {allOutputs.map(output => {
        const id = output.id || output.name || output.platform;
        return (
          <OutputConfigRow
            key={id}
            output={output}
            config={outputConfig[id] || {}}
            onUpdate={(cfg) => handleUpdate(id, cfg)}
            compact={compact}
          />
        );
      })}
    </div>
  );
}
