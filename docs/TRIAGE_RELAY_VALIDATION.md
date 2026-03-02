# Triage Note: Strict Relay Validation Blocker (2026-03-01)

## What Changed
Relay activation IPC contract is fully implemented (bridge -> plugin -> Rust core). However, strict automated validation of the `relay_start` and `relay_stop` actions is currently blocked by local environment conditions that prevent the generation of fresh OBS logs at/after run start time.

## Blocker and Risk
- **Blocker:** `run-strict-cycle.ps1` often finds no new OBS log after launch, or hits `spawn EPERM` during dock bundle builds.
- **Risk:** Without strict validation, we cannot guarantee that the terminal `relay_action_result` is being correctly observed by the shim in a live environment, leading to potential silent timeouts.

## Recommended Validation Policy (Short)
We should employ a **Filtered Fallback Strategy** to unblock development while maintaining signal:

1.  **Enable Fallback Mode:** Use `-ValidateAllowAfterTimestampFallback` when fresh logs are missing.
2.  **Strict Filtering:** Always use `-RequestId` and `-ActionType` filters to ensure the validator isn't matching stale evidence from previous successful runs.
3.  **Timeout Guard:** Always use `-ForbidCompletionTimeout` to ensure we aren't just matching the "queued" state and then timing out.
4.  **Fail-Fast on Missing ID:** If the fallback log exists but lacks the specific `requestId`, the validator should emit `NonRetryable: fallback_log_missing_request_id` and stop immediately (avoiding wasted retry cycles).

## Recommended Minimal Acceptance Criteria
Relay action validation is "good enough" if:
- A single `relay_start` / `relay_stop` cycle completes with `ok: true` and a non-timeout status in the latest available log.
- The `requestId` in the log matches the current session's generated ID.
- The `relay action result resolved` log line is observed in the plugin output.

## Next 3 Actions
1.  **Execute Single-Action Strict Run:** Run `run-strict-cycle.ps1 -SelfTestActionJson ... -ValidateAllowAfterTimestampFallback -ValidateForbidCompletionTimeout`.
2.  **Monitor for Stale-Fallback Signal:** If `NonRetryable` is hit, manually verify if OBS is even launching/logging or if the logs are being written to an unexpected path.
3.  **Trace Core Emission:** If timeouts persist in logs despite the relay starting, manually verify the Rust core's `relay_action_result` emission log line.
