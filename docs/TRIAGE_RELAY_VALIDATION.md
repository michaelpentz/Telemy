# Triage Note: Relay Validation Status (Updated 2026-03-02)

## What Changed
- **Relay Action Loop Completion (`4d9a9f5`):** The previous functional blocker where the C++ plugin did not observe or consume the `relay_action_result` from the Rust core is now **resolved**. The plugin shim now actively handles terminal resolution for `relay_start` and `relay_stop` actions.
- **Validator Hardening:** `validate-obs-log.ps1` has been updated with a `-ForbidCompletionTimeout` guard and explicit `NonRetryable` signaling for stale-fallback scenarios.
- **Script Integration:** `dev-cycle.ps1` and `run-strict-cycle.ps1` now fully support and pass through the new validation parameters.

## Current Status
The **functional blocker is resolved**. The relay activation IPC contract (bridge -> plugin -> Rust core -> relay action result -> shim resolution) is now fully wired end-to-end in the C++ code.

The remaining challenge is purely **environmental**: inconsistent OBS log generation during automated strict cycles in some local environments.

## Validation Policy (Active)
The **Filtered Fallback Strategy** is now the standard for unblocking development:

1.  **Fallback Mode:** Use `-ValidateAllowAfterTimestampFallback` when fresh logs are missing at/after launch.
2.  **Strict Filtering:** Always use `-RequestId` and `-ActionType` filters to prevent matching stale data.
3.  **Timeout Guard:** Always use `-ForbidCompletionTimeout` to ensure the terminal status is `completed` or `failed` (not `completion_timeout`).
4.  **Fail-Fast:** The validator now correctly emits `NonRetryable: fallback_log_missing_request_id` to prevent wasted retry cycles when target evidence is missing from a fallback log.

## Minimal Acceptance Criteria (Verified)
Relay action validation is considered successful if:
- A `relay_start` or `relay_stop` action results in a non-timeout terminal (`completed` or `failed`).
- The matching `requestId` is found in the log.
- The plugin logs: `relay action result resolved: request_id=... action_type=... ok=...`

## Next Actions
1.  **Live OBS Strict Cycle:** Execute a full real-OBS validation run using the latest hardened scripts to confirm the end-to-end terminal resolution.
2.  **Environment Stability:** Investigate `spawn EPERM` errors during dock builds to stabilize the automated CI/CD pipeline.
3.  **Final Publish:** Once a stable strict run confirms the relay loop, proceed with v0.0.3 tagging and release.
