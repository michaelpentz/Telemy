# Dock Code Split Design

**Date:** 2026-03-04
**Status:** Approved

## Goal

Split the 3060-line `aegis-dock.jsx` monolith into focused modules. All dock source files consolidate into `obs-plugin/dock/` as the single source of truth. Root-level copies and the sync script are eliminated.

## File Structure

```
telemy-v0.0.4/obs-plugin/dock/
├── aegis-dock.jsx                    # Root component (~800 lines) — layout, sections, render
├── aegis-dock-entry.jsx              # ReactDOM mount (exists, unchanged)
├── aegis-dock-bridge.js              # Bridge state (exists, unchanged)
├── aegis-dock-bridge-host.js         # Bridge host adapter (exists, unchanged)
├── aegis-dock-browser-host-bootstrap.js  # Native API bootstrap (exists, unchanged)
├── aegis-dock-app.js                 # Built bundle (esbuild output)
├── aegis-dock.html                   # Dock HTML shell (exists, unchanged)
├── constants.js                      # ENGINE_STATES, colors, defaults, storage keys, sim data
├── utils.js                          # Color, time, name-matching, localStorage, intent helpers
├── css.js                            # getDockCss() theme stylesheet generator
├── hooks.js                          # useAnimatedValue, useRollingMaxBitrate, useDockCompactMode
├── use-dock-state.js                 # useDockState — bridge integration + event listeners
├── use-simulated-state.js            # useSimulatedState — dev/preview fallback
├── ui-components.jsx                 # Section, StatusDot, BitrateBar, StatPill, ToggleRow, ConnectionCard, EngineStateChips
├── scene-components.jsx              # SceneButton, scene rule editor rows, color picker, threshold input
├── encoder-components.jsx            # OutputBar, EncoderGroupHeader, HiddenOutputsToggle, OutputConfigRow, OutputConfigPanel
└── relay-section.jsx                 # Relay state machine UI (unlicensed/idle/activating/active/error), relay stats, connection URLs
```

## Module Boundaries

### constants.js (~120 lines)
- `ENGINE_STATES`, `HEALTH_COLORS`, `SCENE_INTENT_COLORS`, `PIPE_STATUS_COLORS`, `SETTING_COLORS`
- `UI_ACTION_COOLDOWNS_MS`, `AUTO_SWITCH_LOCK_TIMEOUT_MS`
- `OUTPUT_HEALTH_COLORS`, `getOutputHealthColor()`
- Storage keys: `SCENE_LINK_STORAGE_KEY`, etc.
- `DEFAULT_AUTO_SCENE_RULES`, `RULE_BG_PRESETS`, `SCENE_PROFILE_NAME_HINTS`
- `OBS_YAMI_GREY_DEFAULTS`
- `SIM_SCENES`, `SIM_SETTING_DEFS`, `SIM_EVENTS`

### utils.js (~200 lines)
- `genRequestId()`, `formatTime()`, `parseHexColor()`, `toRgba()`, `isLightColor()`
- `normalizeOptionalHexColor()`, `getDefaultRuleBgColor()`, `normalizeIntent()`, `inferIntentFromName()`
- `normalizeSceneName()`, `findBestSceneIdForRule()`, `mapRelayStatusForUi()`
- `loadSceneIntentLinks()`, `loadSceneIntentLinkNames()`, `findSceneIdByName()`, `normalizeLinkMap()`
- `loadAutoSceneRules()`, `normalizeAutoSceneRulesValue()`
- `cefCopyToClipboard()`

### css.js (~30 lines)
- `getDockCss(theme)` — generates dynamic CSS for OBS CEF host

### hooks.js (~100 lines)
- `useAnimatedValue(target, duration)` — RAF-based smooth value transitions
- `useRollingMaxBitrate(outputItems)` — rolling max with 0.2% decay
- `useDockCompactMode(containerRef)` — ResizeObserver responsive layout

### use-dock-state.js (~160 lines)
- `useDockState()` — bridge polling, action dispatch, event listeners, scene recovery

### use-simulated-state.js (~150 lines)
- `useSimulatedState()` — simulation layer mirroring exact bridge shape

### ui-components.jsx (~250 lines)
- `Section` — collapsible section with badge
- `StatusDot` — pulsing indicator
- `BitrateBar` — horizontal progress bar
- `StatPill` — compact metric display
- `ToggleRow` — toggle switch row
- `ConnectionCard` — connection with signal bars
- `EngineStateChips` — engine state grid

### scene-components.jsx (~350 lines)
- `SceneButton` — scene selector with intent color
- Scene rule compact row + expanded edit grid (extracted from AegisDock render)
- Color picker, threshold input, scene dropdown elements

### encoder-components.jsx (~250 lines)
- `OutputBar` — single output health bar
- `EncoderGroupHeader` — encoder group divider
- `HiddenOutputsToggle` — collapsible hidden outputs
- `OutputConfigRow` — per-output name/group/visibility
- `OutputConfigPanel` — container for config rows

### relay-section.jsx (~250 lines)
- Relay state machine rendering (unlicensed/idle/activating/active/error states)
- Relay ingest stats panel (bitrate bar + RTT/latency/loss pills)
- Click-to-copy connection URLs (Ingest/Stream/Play)

### aegis-dock.jsx (~800 lines, down from 3060)
- Imports from all modules above
- `AegisDock` component: state management, derived state, effects, handlers, top-level layout
- Sections rendered inline using imported components

## Build Changes

### esbuild command (updated path)
```bash
cd E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock && \
NODE_PATH=../../dock-preview/node_modules npx esbuild \
  aegis-dock-entry.jsx --bundle --format=iife --jsx=automatic \
  --outfile=aegis-dock-app.js --target=es2020 --minify
```

Note: `NODE_PATH` points to `dock-preview/node_modules` for react/react-dom (relative from new location).

### C++ dock host
Update `obs_browser_dock_host_scaffold.cpp` asset path from `E:/Code/telemyapp/` to `E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock/`.

### dock-preview Vite config
Update `@dock` alias from `E:/Code/telemyapp/` to `E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock/`.

## Cleanup

- Delete root-level copies: `aegis-dock.jsx`, `aegis-dock-entry.jsx`, `aegis-dock-bridge.js`, `aegis-dock-bridge-host.js`, `aegis-dock-browser-host-bootstrap.js`, `aegis-dock-app.js`, `aegis-dock.html`
- Delete `obs-plugin/dock/sync-dock.sh`

## Verification

1. Build the bundle from new location
2. Diff old vs new `aegis-dock-app.js` — should be functionally identical (minified output may differ in variable names but behavior is the same)
3. Restart OBS, verify dock loads and renders correctly
4. Verify dock-preview still works (`npm run dev` from `dock-preview/`)

## Constraints Preserved

- OBS CEF flat-div rendering pattern — no structural changes to JSX output
- `--jsx=automatic` required (no React import in source)
- All exports/imports use ES module syntax (esbuild handles bundling)
- No changes to bridge contract or C++ snapshot format
