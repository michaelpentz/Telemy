# Relay Provision Progress — Design

**Date:** 2026-03-07
**Goal:** Show accurate step-by-step relay provisioning progress in the OBS dock UI, so the relay is truly ready when the dock shows ACTIVE.

## Problem

Current flow: `POST /relay/start` blocks ~16s for EC2 launch, returns `status: "active"`. But relay containers need another ~15-30s to start. The dock jumps from "Provisioning" to "Active" before the relay can accept connections, causing IRL Pro timeouts.

## Architecture

### Async Provisioning (Go Control Plane)

`POST /relay/start` returns immediately with `status: "provisioning"`, `provision_step: "launching_instance"`. A background goroutine handles the actual provisioning and updates the session's `provision_step` in the DB as each phase completes.

`GET /relay/active` returns the current `provision_step` on each poll.

When the final step succeeds, session status flips to `"active"` and the DNS record is created (hostname only resolves when truly ready).

### Provision Steps

| Step | Detection | Dock Label |
|------|-----------|------------|
| `launching_instance` | RunInstances called | Launching instance... |
| `waiting_for_instance` | EC2 DescribeInstances reports running | Waiting for instance... |
| `starting_docker` | Instance running, probing port 8090 | Starting services... |
| `starting_containers` | Port 8090 responds | Starting containers... |
| `creating_stream` | `/health` returns OK | Creating stream... |
| `ready` | Stream endpoint returns data, status -> active | Ready |

### Failure Handling

If any step exceeds a 3-minute total deadline, the goroutine sets `status: "failed"`, `provision_step: "timeout_<step>"`, terminates the EC2 instance, and cleans up the session.

### DB Change

Add `provision_step VARCHAR(32)` column to `sessions` table.

## C++ Plugin — Poll Loop

1. Plugin calls `POST /relay/start` — returns almost instantly with `status: "provisioning"`
2. Plugin emits `relay_start` action result with `status=provisioning` (dock enters provisioning UI)
3. Plugin polls `GET /relay/active` every 2s
4. Each response includes `provision_step` — plugin emits `relay_provision_progress` dock event:
   ```json
   {
     "actionType": "relay_provision_progress",
     "step": "starting_containers",
     "stepNumber": 4,
     "totalSteps": 6,
     "label": "Starting containers..."
   }
   ```
5. When `status` becomes `"active"`, plugin emits final `relay_start` completed result
6. Heartbeat loop starts only after `status=active`
7. Plugin gives up after 3 minutes (matches server deadline)

Step labels are defined in C++ (display strings stay out of the API).

## Dock UI — Progress Display

Replaces the static "Provisioning relay... this may take a few minutes" message.

Components:
- Step label text (e.g., "Starting containers...")
- Step counter (e.g., "4 / 6")
- Thin progress bar filling proportionally
- Pulsing animation on current step label

Bridge state:
- New `relayProvisionStep` field: `{ step, stepNumber, totalSteps, label }`
- Updated by `relay_provision_progress` events
- Cleared on `relay_start` completion or failure
- Falls back to static "Provisioning relay..." if no step data (backward compat)

## Files Changed

| Layer | Files | Scope |
|-------|-------|-------|
| DB | `migrations/004_add_provision_step.sql` | Add column |
| Go | `handlers.go`, `store.go`, `model/types.go` | Async provisioning, health probes, step updates |
| C++ | `obs_plugin_entry.cpp` | Poll loop, step event emission |
| Dock bridge | `aegis-dock-bridge.js` | Handle `relay_provision_progress` |
| Dock UI | `aegis-dock.jsx` | Progress bar component |

## Not Included (YAGNI)

- No WebSocket/SSE — polling every 2s is fine for a ~30s process
- No estimated time remaining — step counter is sufficient
- No per-step retry — if a step fails, the whole provisioning fails
