# Design: Encoders & Uploads Dock Section

Date: 2026-03-01
Status: Approved
Spec reference: `docs/API_SPEC_v1.md` Section 13

## Purpose

Add a new "Encoders & Uploads" section to the OBS dock (`aegis-dock.jsx`) that shows per-output streaming health with gradient bitrate bars. Targets multi-encode setups (e.g., horizontal 1080p for Twitch/Kick/YouTube + vertical 1080x1920 for TikTok/YT Shorts) but works cleanly for single-output setups too.

## Data Source

Backend IPC and bridge projection are already complete:

- Rust `SnapshotOutput`: `id, name, active, bitrate_kbps, drop_pct, fps, encoding_lag_ms, encoder, group, resolution, hidden`
- Bridge `getState().outputs` = `{ groups: [{ name, encoder, resolution, totalBitrateKbps, avgLagMs, items: [...] }], hidden: [...] }`
- Bridge `getState().bitrate.outputs` = flat normalized array (alternate access path)

No backend or bridge changes needed.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Placement | After Bitrate, before Relay | Keeps streaming data contiguous |
| Default state | Always open (no collapse) | Streamer wants all output health visible at a glance |
| Ungrouped outputs | Flat list, no header | Clean for single-encode setups; no meaningless "Ungrouped" card |
| Hidden outputs | Count + toggle row | "Hidden (2) [Show]" expands dimmed name list |
| Bar color | Health gradient relative to target | Adapts per-output (TikTok 2.5 Mbps can be green at target) |
| Info layout | Stats above bar | Name + bitrate + fps + drop% on one line, gradient bar below |

## Layout

### Always visible (no collapse toggle)

When encoder groups exist, lightweight divider headers separate groups:

```
⊞ ENCODERS & UPLOADS [5]

  ── Horizontal  1920×1080 ──────────
     Pool 18.5 Mbps  •  Lag 2.1ms

  Twitch     6.2 Mbps  60fps  0.01%
  ████████████████████░░░░░░░░░░░░

  Kick       4.5 Mbps  60fps  0.02%
  █████████████████░░░░░░░░░░░░░░░

  YT Horiz   6.2 Mbps  60fps  0.01%
  ████████████████████░░░░░░░░░░░░

  ── Vertical  1080×1920 ────────────
     Pool 8.4 Mbps  •  Lag 3.0ms

  TikTok     2.5 Mbps  30fps  0.03%
  ██████████░░░░░░░░░░░░░░░░░░░░░░

  YT Shorts  2.5 Mbps  30fps  0.02%
  ██████████░░░░░░░░░░░░░░░░░░░░░░

  Hidden (2) [Show]
```

When no groups are assigned, renders as a flat list with no dividers.

## Gradient Bar Spec

### Health color thresholds (relative to target bitrate)

| Range | Color | Hex |
|-------|-------|-----|
| 90%+ of target | Green | `#2ea043` |
| 70-90% | Yellow-green to amber | `#8ac926` to `#d29922` |
| 50-70% | Amber to orange | `#d29922` to `#e85d04` |
| Below 50% | Red | `#da3633` |

### Target bitrate source

Use the max observed bitrate per output as a rolling baseline (self-calibrating). No user config required. Future: config-defined targets can override.

### Bar styling

- Height: 4px (compact: 3px)
- Border-radius: 2px
- Background track: `var(--theme-border)`
- Fill: solid health color (one color per output, not a CSS gradient on the bar)
- Glow: `box-shadow: 0 0 4px ${healthColor}30`
- Animated width via `useAnimatedValue`

### Inactive outputs

Bar empty (0 width), metrics show "—", entire row at 40% opacity.

## Components

Three new components in `aegis-dock.jsx`:

### 1. `OutputBar`

Single output row: info line + gradient bar.

Props: `name, bitrateKbps, fps, dropPct, lagMs, active, healthColor, maxBitrate, compact`

- Info line: flex row — name left-aligned, `X.X Mbps  XXfps  X.XX%` right-aligned
- Bar below: animated fill width + health-derived color
- Inactive state: dimmed, empty bar, "—" metrics
- Font: `var(--theme-font-family)`, 10px (compact: 9px)

### 2. `EncoderGroupHeader`

Lightweight divider when groups exist.

Props: `name, resolution, totalBitrateKbps, avgLagMs, compact`

- Thin rule line with group name + resolution tag inline
- Below: pool bitrate + avg lag in muted text
- Skipped entirely when group name is "Ungrouped"

### 3. `HiddenOutputsToggle`

"Hidden (N) [Show]" toggle row.

Props: `items, compact`

- Collapsed: single line with count + clickable [Show] text
- Expanded: dimmed list of hidden output names (no bars, no metrics)
- Not rendered when `items.length === 0`

## Data Flow

```
AegisDock top-level:
  const outputs = ds.outputs || { groups: [], hidden: [] };
  const allOutputs = outputs.groups?.flatMap(g => g.items) || [];
  const activeCount = allOutputs.filter(o => o.active).length;

Section rendering:
  outputs.groups.map(group =>
    group.name !== "Ungrouped"
      ? <EncoderGroupHeader {...group} />
      : null
    group.items.map(item =>
      <OutputBar {...item} maxBitrate={sectionMax} />
    )
  )
  outputs.hidden.length > 0
    ? <HiddenOutputsToggle items={outputs.hidden} />
    : null
```

## Theme Integration

All colors via `var(--theme-*, fallback)` matching existing dock patterns. Health colors (green/amber/red) are hardcoded hex constants (not theme-derived), consistent with `HEALTH_COLORS` and `PIPE_STATUS_COLORS`.

## Compact Mode

`isCompact` and `isUltraCompact` flow through props:

| Element | Regular | Compact |
|---------|---------|---------|
| Info line font | 10px | 9px |
| Bar height | 4px | 3px |
| Row gap | 8px | 6px |
| Group header font | 9px | 8px |
| Pool stats font | 9px | 8px |

## Simulation Data

When bridge is unavailable (`!bridgeAvailable`), the simulated state hook should include mock `outputs` data with 2 groups + 2 hidden items, matching the layout above.

## Section Visibility

Always rendered when `allOutputs.length > 0`. No dependency on relay state or mode — outputs exist regardless of IRL/STUDIO mode.

## Out of Scope

- Settings UI for output rename/group/visibility (separate task)
- Config-defined target bitrates (future enhancement; self-calibrating rolling max for now)
- Per-link relay telemetry (Section 12, separate feature)
