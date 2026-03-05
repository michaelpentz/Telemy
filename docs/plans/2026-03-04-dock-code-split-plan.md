# Dock Code Split Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Split the 3060-line `aegis-dock.jsx` monolith into focused modules, consolidate all dock source into `obs-plugin/dock/` as the single source of truth.

**Architecture:** Extract constants, utilities, CSS, hooks, and component groups into separate files. The root `AegisDock` component stays in `aegis-dock.jsx` and imports everything. esbuild bundles it all back into one `aegis-dock-app.js` for OBS CEF.

**Tech Stack:** React (JSX), esbuild (bundler), OBS CEF (runtime)

**Design doc:** `docs/plans/2026-03-04-dock-code-split-design.md`

---

## Reference: Current File Structure

The source file is `E:\Code\telemyapp\aegis-dock.jsx` (3060 lines). Key section boundaries:

| Lines | Section |
|-------|---------|
| 32-123 | Constants (ENGINE_STATES, colors, defaults, sim data) |
| 126-164 | Theme defaults (OBS_YAMI_GREY_DEFAULTS, SIM_*) |
| 168-192 | CSS (getDockCss) |
| 196-391 | Utilities (color, time, name-matching, localStorage) |
| 395-420 | useAnimatedValue hook |
| 422-442 | useRollingMaxBitrate hook |
| 444-471 | useDockCompactMode hook |
| 480-637 | useDockState hook |
| 643-795 | useSimulatedState hook |
| 799-859 | Section component |
| 862-878 | StatusDot component |
| 881-901 | BitrateBar component |
| 905-950 | OutputBar component |
| 954-988 | EncoderGroupHeader component |
| 992-1030 | HiddenOutputsToggle component |
| 1034-1144 | OutputConfigRow component |
| 1149-1214 | OutputConfigPanel component |
| 1219-1267 | SceneButton component |
| 1270-1280 | StatPill component |
| 1284-1324 | ConnectionCard component |
| 1327-1353 | EngineStateChips component |
| 1355-1367 | cefCopyToClipboard utility |
| 1370-1400 | ToggleRow component |
| 1403-3060 | AegisDock main component |

All files will be created in `E:\Code\telemyapp\telemy-v0.0.4\obs-plugin\dock\`.

---

### Task 1: Extract constants.js

**Files:**
- Create: `obs-plugin/dock/constants.js`
- Modify: `aegis-dock.jsx` (root copy, lines 32-164)

**Step 1: Create constants.js**

Extract from `aegis-dock.jsx` lines 32-164 into `obs-plugin/dock/constants.js`. This includes:
- `ENGINE_STATES` (lines 33-39)
- `HEALTH_COLORS` (lines 41-45)
- `SCENE_INTENT_COLORS` (lines 47-52)
- `PIPE_STATUS_COLORS` (lines 54-58)
- `SETTING_COLORS` (lines 61-67)
- `UI_ACTION_COOLDOWNS_MS` (lines 70-75)
- `AUTO_SWITCH_LOCK_TIMEOUT_MS` (line 76)
- `OUTPUT_HEALTH_COLORS` (lines 78-84)
- `getOutputHealthColor()` (lines 86-94)
- Storage keys (lines 96-99)
- `DEFAULT_AUTO_SCENE_RULES` (lines 101-107)
- `RULE_BG_PRESETS` (lines 109-115)
- `SCENE_PROFILE_NAME_HINTS` (lines 117-123)
- `OBS_YAMI_GREY_DEFAULTS` (lines 129-139)
- `SIM_SCENES` (lines 142-148)
- `SIM_SETTING_DEFS` (lines 150-156)
- `SIM_EVENTS` (lines 158-164)

All values are `export const`. No dependencies on other modules.

**Step 2: Verify no circular dependencies**

Confirm none of these constants import from any other dock module. They are all pure data — no imports needed.

---

### Task 2: Extract utils.js

**Files:**
- Create: `obs-plugin/dock/utils.js`

**Step 1: Create utils.js**

Extract from `aegis-dock.jsx` lines 196-391 plus `cefCopyToClipboard` (lines 1355-1367). All functions become named exports.

Import needed constants at top:
```js
import { DEFAULT_AUTO_SCENE_RULES, RULE_BG_PRESETS, SCENE_PROFILE_NAME_HINTS, SCENE_LINK_STORAGE_KEY, SCENE_LINK_NAME_STORAGE_KEY, AUTO_SCENE_RULES_STORAGE_KEY } from "./constants.js";
```

Functions to export:
- `genRequestId`
- `formatTime`
- `parseHexColor`
- `toRgba`
- `isLightColor`
- `normalizeOptionalHexColor`
- `getDefaultRuleBgColor` (uses `RULE_BG_PRESETS`)
- `normalizeIntent`
- `inferIntentFromName`
- `normalizeSceneName`
- `findBestSceneIdForRule` (uses `SCENE_PROFILE_NAME_HINTS`)
- `mapRelayStatusForUi`
- `loadSceneIntentLinks` (uses `SCENE_LINK_STORAGE_KEY`)
- `loadSceneIntentLinkNames` (uses `SCENE_LINK_NAME_STORAGE_KEY`)
- `findSceneIdByName`
- `normalizeLinkMap`
- `loadAutoSceneRules` (uses `AUTO_SCENE_RULES_STORAGE_KEY`, `DEFAULT_AUTO_SCENE_RULES`)
- `normalizeAutoSceneRulesValue` (uses `DEFAULT_AUTO_SCENE_RULES`)
- `cefCopyToClipboard`

---

### Task 3: Extract css.js

**Files:**
- Create: `obs-plugin/dock/css.js`

**Step 1: Create css.js**

Extract `getDockCss(theme)` function (lines 171-192) from `aegis-dock.jsx`.

```js
export function getDockCss(theme) {
  // ... exact content from lines 171-192
}
```

No imports needed — the function only uses its `theme` parameter.

---

### Task 4: Extract hooks.js

**Files:**
- Create: `obs-plugin/dock/hooks.js`

**Step 1: Create hooks.js**

Extract three small hooks (lines 399-471):
- `useAnimatedValue(target, duration)` (lines 399-420)
- `useRollingMaxBitrate(outputItems)` (lines 422-442)
- `useDockCompactMode(containerRef)` (lines 444-471)

Top imports:
```js
import { useState, useEffect, useRef, useCallback } from "react";
```

All three become named exports.

---

### Task 5: Extract use-dock-state.js

**Files:**
- Create: `obs-plugin/dock/use-dock-state.js`

**Step 1: Create use-dock-state.js**

Extract `useDockState()` hook (lines 480-637).

Top imports:
```js
import { useState, useEffect, useRef, useCallback } from "react";
import { genRequestId } from "./utils.js";
```

Check lines 480-637 for any references to constants. The hook references `window.aegisDockNative` directly (bridge API), and may use `genRequestId` for action dispatch.

Single named export: `export function useDockState() { ... }`

---

### Task 6: Extract use-simulated-state.js

**Files:**
- Create: `obs-plugin/dock/use-simulated-state.js`

**Step 1: Create use-simulated-state.js**

Extract `useSimulatedState()` hook (lines 643-795).

Top imports:
```js
import { useState, useEffect, useRef, useCallback } from "react";
import { SIM_SCENES, SIM_SETTING_DEFS, SIM_EVENTS, OBS_YAMI_GREY_DEFAULTS, DEFAULT_AUTO_SCENE_RULES } from "./constants.js";
```

Check lines 643-795 for exact constant references. This hook builds a simulated bridge state using the SIM_* constants.

Single named export: `export function useSimulatedState() { ... }`

---

### Task 7: Extract ui-components.jsx

**Files:**
- Create: `obs-plugin/dock/ui-components.jsx`

**Step 1: Create ui-components.jsx**

Extract these components:
- `Section` (lines 803-859)
- `StatusDot` (lines 862-878)
- `BitrateBar` (lines 881-901)
- `StatPill` (lines 1270-1280)
- `ToggleRow` (lines 1370-1400)
- `ConnectionCard` (lines 1284-1324)
- `EngineStateChips` (lines 1327-1353)

Top imports:
```js
import { useState } from "react";
import { ENGINE_STATES } from "./constants.js";
```

Check each component for references to constants or utilities. `EngineStateChips` uses `ENGINE_STATES`. `Section` uses `useState` for open/closed toggle.

All components are named exports.

---

### Task 8: Extract scene-components.jsx

**Files:**
- Create: `obs-plugin/dock/scene-components.jsx`

**Step 1: Create scene-components.jsx**

Extract:
- `SceneButton` (lines 1219-1267)

This component uses `SCENE_INTENT_COLORS` and `toRgba` for intent-based coloring.

Top imports:
```js
import { SCENE_INTENT_COLORS } from "./constants.js";
import { toRgba } from "./utils.js";
```

Named export: `export function SceneButton({ ... }) { ... }`

Note: The scene rule editor JSX is inline in the AegisDock render method. Extracting it would require passing many state variables as props. **Leave the scene rule editor inline in aegis-dock.jsx for now** — extracting it later is straightforward once the foundation is split.

---

### Task 9: Extract encoder-components.jsx

**Files:**
- Create: `obs-plugin/dock/encoder-components.jsx`

**Step 1: Create encoder-components.jsx**

Extract:
- `OutputBar` (lines 905-950)
- `EncoderGroupHeader` (lines 954-988)
- `HiddenOutputsToggle` (lines 992-1030)
- `OutputConfigRow` (lines 1034-1144)
- `OutputConfigPanel` (lines 1149-1214)

Top imports:
```js
import { useState } from "react";
import { OUTPUT_HEALTH_COLORS, getOutputHealthColor } from "./constants.js";
import { toRgba } from "./utils.js";
```

Check each component for exact constant/utility references. `OutputBar` uses `getOutputHealthColor`. `OutputConfigRow` and `OutputConfigPanel` use `useState`.

All components are named exports.

---

### Task 10: Extract relay-section.jsx

**Files:**
- Create: `obs-plugin/dock/relay-section.jsx`

**Step 1: Evaluate relay section extraction**

The relay section rendering (lines ~2701-2920 in AegisDock) is deeply interleaved with AegisDock state (`relayActivating`, `relayError`, `handleRelayToggle`, many derived values). Extracting it as a standalone component would require passing 15+ props.

**Decision:** Leave relay rendering inline in `aegis-dock.jsx`. The component is already well-organized with section comments. The value of extraction doesn't justify the prop-threading complexity.

**Skip this task.** Create an empty `relay-section.jsx` placeholder only if we want it for future extraction — but per YAGNI, don't.

---

### Task 11: Wire imports in aegis-dock.jsx

**Files:**
- Modify: `obs-plugin/dock/aegis-dock.jsx` (the git-tracked copy)

**Step 1: Copy root aegis-dock.jsx to obs-plugin/dock/**

The git-tracked copy at `obs-plugin/dock/aegis-dock.jsx` should already be current. Verify it matches the root copy:
```bash
diff E:/Code/telemyapp/aegis-dock.jsx E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock/aegis-dock.jsx
```

If they differ, copy root → repo.

**Step 2: Add imports to top of aegis-dock.jsx**

Replace the extracted code blocks with imports:

```js
import { useState, useEffect, useRef, useCallback, useMemo } from "react";
import {
  ENGINE_STATES, HEALTH_COLORS, SCENE_INTENT_COLORS, PIPE_STATUS_COLORS,
  SETTING_COLORS, UI_ACTION_COOLDOWNS_MS, AUTO_SWITCH_LOCK_TIMEOUT_MS,
  OUTPUT_HEALTH_COLORS, getOutputHealthColor,
  SCENE_LINK_STORAGE_KEY, SCENE_LINK_NAME_STORAGE_KEY,
  AUTO_SCENE_RULES_STORAGE_KEY, OUTPUT_CONFIG_STORAGE_KEY,
  DEFAULT_AUTO_SCENE_RULES, RULE_BG_PRESETS, SCENE_PROFILE_NAME_HINTS,
  OBS_YAMI_GREY_DEFAULTS, SIM_SCENES, SIM_SETTING_DEFS, SIM_EVENTS
} from "./constants.js";
import {
  genRequestId, formatTime, parseHexColor, toRgba, isLightColor,
  normalizeOptionalHexColor, getDefaultRuleBgColor, normalizeIntent,
  inferIntentFromName, normalizeSceneName, findBestSceneIdForRule,
  mapRelayStatusForUi, loadSceneIntentLinks, loadSceneIntentLinkNames,
  findSceneIdByName, normalizeLinkMap, loadAutoSceneRules,
  normalizeAutoSceneRulesValue, cefCopyToClipboard
} from "./utils.js";
import { getDockCss } from "./css.js";
import { useAnimatedValue, useRollingMaxBitrate, useDockCompactMode } from "./hooks.js";
import { useDockState } from "./use-dock-state.js";
import { useSimulatedState } from "./use-simulated-state.js";
import {
  Section, StatusDot, BitrateBar, StatPill, ToggleRow,
  ConnectionCard, EngineStateChips
} from "./ui-components.jsx";
import { SceneButton } from "./scene-components.jsx";
import {
  OutputBar, EncoderGroupHeader, HiddenOutputsToggle,
  OutputConfigRow, OutputConfigPanel
} from "./encoder-components.jsx";
```

**Step 3: Delete extracted code blocks**

Remove lines 32-795 (constants, utils, css, hooks) and lines 799-1400 (components, cefCopyToClipboard) from `aegis-dock.jsx`. Keep only:
- The React import (line 1)
- The new import block (step 2)
- The `AegisDock` component (lines 1403-3060)
- The default export

The file should be ~1660 lines (AegisDock component body).

---

### Task 12: Update aegis-dock-entry.jsx

**Files:**
- Modify: `obs-plugin/dock/aegis-dock-entry.jsx`

**Step 1: Verify entry point**

The entry at `obs-plugin/dock/aegis-dock-entry.jsx` already imports `./aegis-dock.jsx` — this should work as-is since the file stays in the same directory. Verify the import path:

```js
import AegisDock from "./aegis-dock.jsx";
```

No changes needed if this is already the case.

---

### Task 13: Build and verify bundle

**Step 1: Build from new location**

Save the old bundle for comparison:
```bash
cp E:/Code/telemyapp/aegis-dock-app.js E:/Code/telemyapp/aegis-dock-app.js.old
```

Build from `obs-plugin/dock/`:
```bash
cd E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock && \
NODE_PATH=../../../dock-preview/node_modules npx esbuild \
  aegis-dock-entry.jsx --bundle --format=iife --jsx=automatic \
  --outfile=aegis-dock-app.js --target=es2020 --minify
```

Note: `NODE_PATH` is relative — `dock-preview/node_modules` is at `E:\Code\telemyapp\dock-preview\node_modules`, which is `../../../dock-preview/node_modules` from `obs-plugin/dock/`.

**Step 2: Verify bundle builds without errors**

esbuild should exit 0 with no warnings. If there are unresolved imports, fix them.

**Step 3: Sanity check bundle size**

```bash
wc -c E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock/aegis-dock-app.js
wc -c E:/Code/telemyapp/aegis-dock-app.js.old
```

Sizes should be roughly similar (within 5%). Exact match is not expected due to module wrapping and minification differences.

---

### Task 14: Update dock-preview Vite config

**Files:**
- Modify: `dock-preview/vite.config.js`

**Step 1: Update @dock alias**

Change the `@dock` alias from parent dir to `obs-plugin/dock/`:

```js
alias: {
  "@dock": path.resolve(__dirname, "../telemy-v0.0.4/obs-plugin/dock"),
},
```

**Step 2: Verify dock-preview dev server**

```bash
cd E:/Code/telemyapp/dock-preview && npx vite --port 5199
```

Should start without errors and render the dock preview at `http://localhost:5199`.

---

### Task 15: Update AEGIS_DOCK_BRIDGE_ROOT env var

The C++ dock host reads dock assets via the `AEGIS_DOCK_BRIDGE_ROOT` environment variable. Currently set to `E:\Code\telemyapp\`.

**Step 1: Document the new env var value**

New value: `E:\Code\telemyapp\telemy-v0.0.4\obs-plugin\dock`

This must be set as a system/user environment variable on the dev machine. The C++ code (`obs_browser_dock_host_scaffold.cpp` line 56) reads it at runtime.

**Step 2: Set the env var**

This is a manual step — set `AEGIS_DOCK_BRIDGE_ROOT` to the new path in Windows environment variables. OBS must be restarted after changing it.

---

### Task 16: Delete root-level dock copies

**Step 1: Remove root-level files**

These files at `E:\Code\telemyapp\` are no longer needed:
- `aegis-dock.jsx`
- `aegis-dock-entry.jsx`
- `aegis-dock-bridge.js`
- `aegis-dock-bridge-host.js`
- `aegis-dock-browser-host-bootstrap.js`
- `aegis-dock-app.js`
- `aegis-dock.html`

```bash
rm E:/Code/telemyapp/aegis-dock.jsx \
   E:/Code/telemyapp/aegis-dock-entry.jsx \
   E:/Code/telemyapp/aegis-dock-bridge.js \
   E:/Code/telemyapp/aegis-dock-bridge-host.js \
   E:/Code/telemyapp/aegis-dock-browser-host-bootstrap.js \
   E:/Code/telemyapp/aegis-dock-app.js \
   E:/Code/telemyapp/aegis-dock.html
```

These files are outside the git repo, so no git operation needed.

**Step 2: Delete sync script**

```bash
rm E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock/sync-dock.sh
```

This is inside the git repo — will show as a deletion in `git status`.

---

### Task 17: Commit

**Step 1: Stage all new and modified files**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4
git add obs-plugin/dock/constants.js \
        obs-plugin/dock/utils.js \
        obs-plugin/dock/css.js \
        obs-plugin/dock/hooks.js \
        obs-plugin/dock/use-dock-state.js \
        obs-plugin/dock/use-simulated-state.js \
        obs-plugin/dock/ui-components.jsx \
        obs-plugin/dock/scene-components.jsx \
        obs-plugin/dock/encoder-components.jsx \
        obs-plugin/dock/aegis-dock.jsx \
        obs-plugin/dock/aegis-dock-entry.jsx \
        obs-plugin/dock/aegis-dock-app.js
git rm obs-plugin/dock/sync-dock.sh
```

**Step 2: Commit**

```bash
git commit -m "refactor: split aegis-dock.jsx into modules, consolidate dock source"
```

---

## Task Summary

| Task | Description | Depends On |
|------|-------------|------------|
| 1 | Extract constants.js | — |
| 2 | Extract utils.js | 1 |
| 3 | Extract css.js | — |
| 4 | Extract hooks.js | — |
| 5 | Extract use-dock-state.js | 2 |
| 6 | Extract use-simulated-state.js | 1 |
| 7 | Extract ui-components.jsx | 1 |
| 8 | Extract scene-components.jsx | 1, 2 |
| 9 | Extract encoder-components.jsx | 1, 2 |
| 10 | ~~Extract relay-section.jsx~~ | SKIPPED (YAGNI) |
| 11 | Wire imports in aegis-dock.jsx | 1-9 |
| 12 | Update aegis-dock-entry.jsx | 11 |
| 13 | Build and verify bundle | 12 |
| 14 | Update dock-preview Vite config | 13 |
| 15 | Update AEGIS_DOCK_BRIDGE_ROOT env | — |
| 16 | Delete root-level dock copies | 13, 15 |
| 17 | Commit | 16 |

Tasks 1, 3, 4 can run in parallel. Tasks 2, 5, 6, 7, 8, 9 can run in parallel after their dependencies. Task 11 waits for all extractions. Tasks 13-17 are sequential.
