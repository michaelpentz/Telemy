import { SCENE_INTENT_COLORS } from "./constants.js";

// --- Scene Button ---
// [PLUGIN] scene list + active scene from OBS callbacks
// [BRIDGE] pendingSceneId tracked by bridge bookkeeping
export function SceneButton({ name, active, pending, intent, compact = false, onClick }) {
  const c = SCENE_INTENT_COLORS[intent] || SCENE_INTENT_COLORS.OFFLINE;
  return (
    <button onClick={onClick} style={{
      width: "100%", padding: compact ? "6px 8px" : "7px 10px",
      border: `1px solid ${active ? c.border : pending ? "var(--theme-accent, #3a3d45)" : "var(--theme-border, #2a2d35)"}`,
      borderRadius: 4, background: active ? c.bg : "var(--theme-surface, #13151a)",
      display: "flex", alignItems: "center", gap: 8, cursor: "pointer",
      transition: "all 0.15s ease", marginBottom: 4,
      boxShadow: active ? `0 0 12px ${c.border}15, inset 0 1px 0 ${c.border}10` : "none",
      ...(pending && !active ? {
        backgroundImage: "linear-gradient(90deg, transparent 0%, rgba(91,163,245,0.04) 50%, transparent 100%)",
        backgroundSize: "200% 100%",
        animation: "shimmer 2s linear infinite",
      } : {}),
    }}
      onMouseEnter={e => { if (!active) e.currentTarget.style.borderColor = "var(--theme-accent, #3a3d45)"; }}
      onMouseLeave={e => { if (!active) e.currentTarget.style.borderColor = pending ? "var(--theme-accent, #3a3d45)" : "var(--theme-border, #2a2d35)"; }}
    >
      <span style={{
        width: 5, height: 5, borderRadius: "50%",
        background: active ? c.border : pending ? "var(--theme-accent, #5ba3f5)" : "var(--theme-border, #3a3d45)",
        boxShadow: active ? `0 0 4px ${c.border}` : pending ? "0 0 4px var(--theme-accent, #5ba3f5)" : "none",
      }} />
      <span style={{
        fontSize: compact ? 10 : 11, fontWeight: active ? 600 : 400,
        color: active ? c.text : pending ? "var(--theme-accent, #5ba3f5)" : "var(--theme-text-muted, #8b8f98)",
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)", flex: 1, textAlign: "left", minWidth: 0,
        overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
      }}>{name}</span>
      {active && (
        <span style={{
          fontSize: compact ? 7 : 8, fontWeight: 700, color: c.border,
          background: `${c.border}15`, padding: compact ? "1px 4px" : "1px 5px",
          borderRadius: 2, textTransform: "uppercase", letterSpacing: "0.08em",
          flexShrink: 0,
        }}>{compact ? "ON" : "ACTIVE"}</span>
      )}
      {pending && !active && (
        <span style={{
          fontSize: compact ? 7 : 8, fontWeight: 700, color: "#5ba3f5",
          background: "#5ba3f515", padding: compact ? "1px 4px" : "1px 5px",
          borderRadius: 2, textTransform: "uppercase", letterSpacing: "0.08em",
          flexShrink: 0,
        }}>{compact ? "..." : "SWITCHING"}</span>
      )}
    </button>
  );
}
