# Encoders & Uploads Section Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a new always-visible "Encoders & Uploads" section to the OBS dock that shows per-output streaming health with gradient bitrate bars, grouped by encoder configuration.

**Architecture:** Three new components (`OutputBar`, `EncoderGroupHeader`, `HiddenOutputsToggle`) added to `aegis-dock.jsx`. Data consumed from the existing `getState().outputs` bridge projection (already wired). The existing "MULTISTREAM OUTPUTS" sub-card in the Bitrate section is removed since this section replaces it. Simulated state extended for preview mode.

**Tech Stack:** React 19 (JSX), inline styles with CSS custom properties, existing `useAnimatedValue` hook.

---

### Task 1: Add health color utility function

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx` (insert after `SETTING_COLORS` block, around line 68)

**Step 1: Add the `getOutputHealthColor` function**

Insert after `UI_ACTION_COOLDOWNS_MS` (line 76):

```jsx
// Health color for per-output bitrate bars (relative to rolling max target)
const OUTPUT_HEALTH_COLORS = {
  healthy:  "#2ea043",
  good:     "#8ac926",
  warning:  "#d29922",
  degraded: "#e85d04",
  critical: "#da3633",
};

function getOutputHealthColor(currentKbps, maxObservedKbps) {
  if (!maxObservedKbps || maxObservedKbps <= 0 || !currentKbps) return OUTPUT_HEALTH_COLORS.critical;
  const pct = currentKbps / maxObservedKbps;
  if (pct >= 0.9) return OUTPUT_HEALTH_COLORS.healthy;
  if (pct >= 0.7) return OUTPUT_HEALTH_COLORS.good;
  if (pct >= 0.5) return OUTPUT_HEALTH_COLORS.warning;
  if (pct >= 0.3) return OUTPUT_HEALTH_COLORS.degraded;
  return OUTPUT_HEALTH_COLORS.critical;
}
```

**Step 2: Verify dock preview still loads**

Run dock-preview dev server, confirm no errors in console, existing sections render.

**Step 3: Commit**

```bash
git add aegis-dock.jsx
git commit -m "feat(dock): add output health color utility for encoder bars"
```

---

### Task 2: Add `OutputBar` component

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx` (insert after `BitrateBar` component, around line 802)

**Step 1: Add the `OutputBar` component**

Insert after the `BitrateBar` closing brace (line 802):

```jsx
// --- Output Bar (per-output bitrate row with health-gradient bar) ---
// [IPC] per-output data from status_snapshot.outputs[]
function OutputBar({ name, bitrateKbps, fps, dropPct, active, maxBitrate, compact = false }) {
  const healthColor = getOutputHealthColor(bitrateKbps, maxBitrate);
  const animBitrate = useAnimatedValue(active ? (bitrateKbps || 0) : 0, 600);
  const pct = maxBitrate > 0 ? Math.min((animBitrate / maxBitrate) * 100, 100) : 0;
  const inactive = !active;

  return (
    <div style={{ marginBottom: compact ? 6 : 8, opacity: inactive ? 0.4 : 1, transition: "opacity 0.3s ease" }}>
      {/* Info line: name left, stats right */}
      <div style={{
        display: "flex", justifyContent: "space-between", alignItems: "baseline",
        marginBottom: 3, fontSize: compact ? 9 : 10,
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
      }}>
        <span style={{
          color: "var(--theme-text, #e0e2e8)", fontWeight: 600,
          overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", flex: 1, marginRight: 8,
        }}>
          {name || "Output"}
        </span>
        <span style={{ color: "var(--theme-text-muted, #8b8f98)", flexShrink: 0, whiteSpace: "nowrap" }}>
          {inactive ? "—" : (
            <>
              {bitrateKbps != null ? `${(bitrateKbps / 1000).toFixed(1)} Mbps` : "—"}
              {"  "}
              {fps != null ? `${Math.round(fps)}fps` : ""}
              {"  "}
              {dropPct != null ? `${dropPct.toFixed(2)}%` : ""}
            </>
          )}
        </span>
      </div>
      {/* Gradient bar */}
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
```

**Step 2: Verify dock preview still loads**

Run dock-preview, confirm no errors. The component is defined but not rendered yet.

**Step 3: Commit**

```bash
git add aegis-dock.jsx
git commit -m "feat(dock): add OutputBar component for per-output health bars"
```

---

### Task 3: Add `EncoderGroupHeader` component

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx` (insert immediately after `OutputBar`)

**Step 1: Add the `EncoderGroupHeader` component**

```jsx
// --- Encoder Group Header (lightweight divider for grouped outputs) ---
function EncoderGroupHeader({ name, resolution, totalBitrateKbps, avgLagMs, compact = false }) {
  if (name === "Ungrouped") return null;
  return (
    <div style={{ marginTop: 8, marginBottom: 6 }}>
      {/* Divider line with group name + resolution */}
      <div style={{
        display: "flex", alignItems: "center", gap: 6,
        fontSize: compact ? 8 : 9,
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
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
      {/* Pool stats */}
      <div style={{
        fontSize: compact ? 8 : 9, color: "var(--theme-text-muted, #6b7080)",
        fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
        textAlign: "center", marginTop: 3,
      }}>
        Pool {totalBitrateKbps != null ? `${(totalBitrateKbps / 1000).toFixed(1)} Mbps` : "—"}
        {"  \u2022  "}
        Lag {avgLagMs != null ? `${avgLagMs.toFixed(1)}ms` : "—"}
      </div>
    </div>
  );
}
```

**Step 2: Verify dock preview still loads**

**Step 3: Commit**

```bash
git add aegis-dock.jsx
git commit -m "feat(dock): add EncoderGroupHeader divider component"
```

---

### Task 4: Add `HiddenOutputsToggle` component

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx` (insert immediately after `EncoderGroupHeader`)

**Step 1: Add the `HiddenOutputsToggle` component**

```jsx
// --- Hidden Outputs Toggle ---
function HiddenOutputsToggle({ items, compact = false }) {
  const [expanded, setExpanded] = useState(false);
  if (!items || items.length === 0) return null;
  return (
    <div style={{ marginTop: 6 }}>
      <div
        onClick={() => setExpanded(v => !v)}
        style={{
          display: "flex", alignItems: "center", gap: 6, cursor: "pointer",
          fontSize: compact ? 8 : 9,
          fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
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
              fontFamily: "var(--theme-font-family, 'JetBrains Mono', monospace)",
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
```

**Step 2: Verify dock preview still loads**

**Step 3: Commit**

```bash
git add aegis-dock.jsx
git commit -m "feat(dock): add HiddenOutputsToggle component"
```

---

### Task 5: Add rolling max bitrate tracker hook

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx` (insert after `useAnimatedValue` hook, around line 395)

**Step 1: Add the `useRollingMaxBitrate` hook**

This hook tracks the max observed bitrate per output for health-color baseline:

```jsx
// Track rolling max bitrate per output for health color calculation
function useRollingMaxBitrate(outputItems) {
  const maxMapRef = useRef({});
  return useMemo(() => {
    const maxMap = maxMapRef.current;
    let sectionMax = 0;
    if (Array.isArray(outputItems)) {
      outputItems.forEach(item => {
        const key = item.id || item.name || item.platform;
        if (key && item.kbps > 0) {
          const prev = maxMap[key] || 0;
          // Allow slow decay so max adjusts if stream config changes
          const decayed = prev > 0 ? prev * 0.998 : 0;
          maxMap[key] = Math.max(decayed, item.kbps);
        }
        if (key && maxMap[key] > sectionMax) sectionMax = maxMap[key];
      });
    }
    maxMapRef.current = maxMap;
    return { maxMap, sectionMax };
  }, [outputItems]);
}
```

**Step 2: Verify dock preview still loads**

**Step 3: Commit**

```bash
git add aegis-dock.jsx
git commit -m "feat(dock): add rolling max bitrate tracker hook"
```

---

### Task 6: Wire up the Encoders & Uploads section in `AegisDock`

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx`
  - Add data extraction near line 987 (where `bitrate` is extracted)
  - Add section JSX at line 2143 (after Bitrate `</Section>`, before Relay)

**Step 1: Add data extraction at top of `AegisDock`**

Near line 1300 (after `outputBitrates` extraction), add:

```jsx
  // Encoders & Uploads — per-output grouped data
  const encoderOutputs = ds.outputs || { groups: [], hidden: [] };
  const allEncoderItems = encoderOutputs.groups?.flatMap(g => g.items) || [];
  const activeOutputCount = allEncoderItems.filter(o => o.active).length;
  const { maxMap: outputMaxMap, sectionMax: outputSectionMax } = useRollingMaxBitrate(allEncoderItems);
```

**Step 2: Add section JSX after Bitrate section**

Insert after line 2143 (`</Section>` closing the Bitrate section), before the Relay section comment:

```jsx
        {/* ----- ENCODERS & UPLOADS (always visible, per-output health) ----- */}
        {allEncoderItems.length > 0 && (
          <Section title="Encoders & Uploads" icon="⊞" defaultOpen={true} compact={isCompact}
            badge={String(activeOutputCount)}
            badgeColor={activeOutputCount > 0 ? "#2ea043" : "var(--theme-border, #3a3d45)"}>
            {encoderOutputs.groups.map((group, gi) => (
              <div key={group.name || gi}>
                <EncoderGroupHeader
                  name={group.name}
                  resolution={group.resolution}
                  totalBitrateKbps={group.totalBitrateKbps}
                  avgLagMs={group.avgLagMs}
                  compact={isCompact}
                />
                {group.items.map((item, ii) => (
                  <OutputBar
                    key={item.id || `${gi}-${ii}`}
                    name={item.name || item.platform}
                    bitrateKbps={item.kbps}
                    fps={item.fps}
                    dropPct={item.dropPct}
                    active={item.active !== false}
                    maxBitrate={outputMaxMap[item.id || item.name || item.platform] || outputSectionMax}
                    compact={isCompact}
                  />
                ))}
              </div>
            ))}
            <HiddenOutputsToggle items={encoderOutputs.hidden} compact={isCompact} />
          </Section>
        )}
```

**Step 3: Verify dock preview renders the new section**

Check that the section appears after Bitrate with mock data (if sim data is wired in Task 7). If no outputs in sim data yet, section won't render — that's expected.

**Step 4: Commit**

```bash
git add aegis-dock.jsx
git commit -m "feat(dock): wire Encoders & Uploads section into dock layout"
```

---

### Task 7: Add simulated outputs to `useSimulatedState`

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx` (in `useSimulatedState`, around line 590)

**Step 1: Add `outputs` to the simulated state object**

Inside the `useMemo` return object in `useSimulatedState` (after the `bitrate` block at ~line 627), add:

```jsx
    outputs: {
      groups: [
        {
          name: "Horizontal",
          encoder: "x264",
          resolution: "1920x1080",
          totalBitrateKbps: Math.floor((sim1 + sim2) * 0.75),
          avgLagMs: 2.1,
          items: [
            { id: "twitch", name: "Twitch", platform: "Twitch", kbps: Math.max(800, Math.floor((sim1 + sim2) * 0.35)), fps: 60, dropPct: 0.01, active: true },
            { id: "kick", name: "Kick", platform: "Kick", kbps: Math.max(600, Math.floor((sim1 + sim2) * 0.22)), fps: 60, dropPct: 0.02, active: simRelayActive },
            { id: "yt_horiz", name: "YT Horizontal", platform: "YouTube", kbps: Math.max(600, Math.floor((sim1 + sim2) * 0.18)), fps: 60, dropPct: 0.01, active: true },
          ],
        },
        {
          name: "Vertical",
          encoder: "x264",
          resolution: "1080x1920",
          totalBitrateKbps: Math.floor((sim1 + sim2) * 0.25),
          avgLagMs: 3.0,
          items: [
            { id: "tiktok", name: "TikTok", platform: "TikTok", kbps: Math.max(300, Math.floor((sim1 + sim2) * 0.13)), fps: 30, dropPct: 0.03, active: true },
            { id: "yt_shorts", name: "YT Shorts", platform: "YT Shorts", kbps: Math.max(300, Math.floor((sim1 + sim2) * 0.12)), fps: 30, dropPct: 0.02, active: true },
          ],
        },
      ],
      hidden: [
        { id: "virtualcam", name: "Virtual Camera", active: false },
        { id: "recording", name: "Recording", active: false },
      ],
    },
```

**Step 2: Verify dock preview shows the Encoders & Uploads section with simulated data**

- 5 outputs visible across 2 groups
- Gradient bars animate with sim data changes
- "Horizontal" and "Vertical" group headers visible
- "Hidden (2) [Show]" toggle works
- Health colors shift as sim values fluctuate

**Step 3: Commit**

```bash
git add aegis-dock.jsx
git commit -m "feat(dock): add simulated multi-encode outputs for preview mode"
```

---

### Task 8: Remove old MULTISTREAM OUTPUTS sub-card from Bitrate section

**Files:**
- Modify: `E:\Code\telemyapp\aegis-dock.jsx` (lines ~2104-2142)

**Step 1: Remove the MULTISTREAM OUTPUTS block**

Delete the entire sub-card from line 2104 (`<div style={{ marginTop: 8,`) through line 2142 (`</div>`). This is the block that starts with the `MULTISTREAM OUTPUTS` header label.

The `outputBitrates` extraction at line 1300 can remain (it's used by `getState().bitrate.outputs` — harmless) or be removed if no other references exist.

**Step 2: Verify Bitrate section still renders correctly without the sub-card**

Bitrate section should still show threshold grid and per-link bars (when relay active). The per-output detail is now in the Encoders & Uploads section.

**Step 3: Commit**

```bash
git add aegis-dock.jsx
git commit -m "refactor(dock): remove MULTISTREAM OUTPUTS sub-card (replaced by Encoders & Uploads section)"
```

---

### Task 9: Visual verification and compact mode testing

**Files:**
- No file changes (verification only)

**Step 1: Preview at regular width (320px)**

Verify:
- Section appears after Bitrate, before Relay
- Group headers render with name + resolution tag + pool stats
- Per-output bars animate smoothly
- Health colors gradient from green to red as sim values change
- Badge shows active count

**Step 2: Preview at compact width (280px)**

Resize dock-preview container to 280px. Verify:
- Fonts shrink (9px info, 3px bars)
- Row gaps tighten
- No overflow or clipping

**Step 3: Preview at ultra-compact width (240px)**

Resize to 240px. Verify no breakage.

**Step 4: Test hidden outputs toggle**

Click [Show] — verify dimmed names appear. Click [Hide] — verify they collapse.

**Step 5: Commit (if any tweaks needed)**

```bash
git add aegis-dock.jsx
git commit -m "fix(dock): compact mode tweaks for Encoders & Uploads section"
```

---

### Task 10: Update coordination docs

**Files:**
- Modify: `E:\Code\telemyapp\CURRENT_STATUS.md`
- Modify: `E:\Code\telemyapp\HANDOFF_STATUS.md`

**Step 1: Add session update to CURRENT_STATUS.md**

Add at the top of the session updates:

```markdown
## Session Update (2026-03-01 — Encoders & Uploads dock section)

### Implemented
- New "Encoders & Uploads" section in `aegis-dock.jsx` after Bitrate, before Relay
- Per-output gradient bitrate bars with health-color coding (green→red relative to rolling max)
- Encoder group headers with resolution tags and pool stats
- Hidden outputs toggle ("Hidden (N) [Show/Hide]")
- Simulated multi-encode data (2 groups, 5 outputs, 2 hidden) for preview mode
- Removed old MULTISTREAM OUTPUTS sub-card from Bitrate section (replaced)
- Compact and ultra-compact responsive modes verified

### Data Source
- Consumes `getState().outputs` from bridge (already wired by Codex)
- No backend or bridge changes needed
```

**Step 2: Update HANDOFF_STATUS.md current priority list**

Mark item 2 ("finish remaining UI wiring for Encoders and Uploads") as done.

**Step 3: Commit coordination docs are outside git repo — no commit needed**
