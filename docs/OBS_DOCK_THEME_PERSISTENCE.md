## OBS Dock Theme Persistence

### Why this matters
When OBS launches while the plugin shim is still waiting on the IPC command pipe, it keeps logging `cmd pipe connect retry (err=5)` and never receives the themed `status_snapshot`. The dock therefore falls back to its default colors every time OBS closes/restarts. The bridge now builds the pipes with an ACL that explicitly includes the running user, so the pipe opens cleanly in both elevated and non‑elevated scenarios and the native theme travels through.

### Procedure to keep the theme
1. Start the Rust bridge before launching OBS so that `\\.\pipe\aegis_cmd_v1` is listening for the plugin.

   ```powershell
   cd E:\code\telemyapp\telemy-v0.0.3\obs-telemetry-bridge
   cargo run --bin obs-telemetry-bridge
   ```

2. Open OBS from the same user account (elevated if you ran the bridge elevated) and let it finish the startup cycle.
3. Confirm the shader theme lines appear once: the OBS log should show `[aegis-obs-shim] obs dock theme cache refresh` followed by a themed `status_snapshot` line instead of a `completion_timeout`.
4. Avoid killing OBS before it has time to emit the themed status snapshot; if you need to stop it abruptly, run `Stop-Process obs64` *after* the theme refresh is logged.

### Troubleshooting
- If the OBS log still shows repeating `cmd pipe connect retry (err=5)`, stop OBS, restart the bridge (make sure it is listening), and then start OBS again.
- In rare cases the ACL build can fail (permission issues). The bridge logs a warning and falls back to default ACL, which recreates the retries; rerunning as administrator usually resolves that.

### Why this is documented here
Keeping the dock theme stable is critical for QA handoffs and demo runs. This document summarizes the new security ACL change and captures the workflow so future testers know the proper startup order.
