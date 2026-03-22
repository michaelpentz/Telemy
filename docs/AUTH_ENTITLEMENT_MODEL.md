# Telemy Auth and Entitlement Model (v0.0.5)

This document defines the recommended access model for the paid managed relay feature.

## Summary

Telemy uses three distinct security layers:

1. **Account / entitlement layer**
   - Controls who is allowed to activate the paid managed relay feature.
   - Enforced by the control plane.

2. **Control-plane API layer**
   - Authenticates the OBS plugin to the Go backend.
   - Uses a backend-issued `cp_access_jwt`.

3. **Relay publish/play layer**
   - Controls who can publish into or play from the relay's SLS streams.
   - Uses a per-user persistent `stream_token`, surfaced as:
     - `live_{stream_token}`
     - `play_{stream_token}`

These layers are intentionally separate. Product access should not depend on custom SRTLA protocol auth.

---

## Recommended User Flow

Primary UX:
1. User clicks **Login** in the plugin.
2. Plugin calls `POST /api/v1/auth/plugin/login/start`.
3. Plugin launches the returned browser URL and polls `POST /api/v1/auth/plugin/login/poll`.
4. Web auth completes the login attempt and the backend issues plugin auth material.
5. Plugin stores the resulting credential in DPAPI-backed vault storage.
6. Relay activation is enabled only when the backend confirms the user is entitled.

Current backend implementation:
- `POST /api/v1/auth/plugin/login/start`
- `POST /api/v1/auth/plugin/login/poll`
- `POST /api/v1/auth/plugin/login/complete` for the future web tier, protected by shared backend auth
- `GET /api/v1/auth/session`
- `POST /api/v1/auth/refresh`
- `POST /api/v1/auth/logout`

Current web completion surface:
- The browser step is currently a temporary operator-assisted page at `telemyapp.com/login/plugin?attempt=...`.
- The operator approves the login attempt against an existing Telemy `user_id`.
- The long-term customer-facing account flow remains a follow-up item; the implemented backend/plugin contract is already compatible with that future replacement.

On relay activation:
1. Plugin calls `POST /api/v1/relay/start` with `cp_access_jwt`.
2. Control plane checks user identity and entitlement.
3. If authorized, the control plane provisions or returns the user's relay session.
4. Session response includes the user's relay endpoint and per-user stream metadata.
5. User configures the sender app with:
   - `srtla://{relay_host}:5000`
   - `streamid=live_{stream_token}`

OBS playback uses:
- `srt://{relay_host}:4000?streamid=play_{stream_token}`

---

## Why This Model

### Security

- **OAuth/account login** gates paid managed relay usage at the correct boundary: the backend.
- **`cp_access_jwt`** protects control-plane API calls from the plugin.
- **`stream_token`** protects publish/play rights inside SLS even if the relay host is known.

This is stronger and more operationally correct than relying on UI gating alone.

### Compatibility

This model is compatible with sender apps that already support:
- `srtla://host:port`
- `streamid`

That includes the current intended workflow with IRL Pro and similar apps. It avoids requiring custom SRTLA registration extensions that third-party apps are unlikely to support.

### Product UX

End users understand:
- sign in
- subscribe
- click activate

They do not want to manage relay infrastructure, relay internals, or custom transport credentials.

---

## What Is Not the Primary Product Gate

### License key as primary UX

License keys are acceptable as an internal fallback or transitional bootstrap, but they are not the recommended long-term primary user flow.

Reasons:
- easy to copy/share
- awkward revocation
- worse UX than browser login

### Custom SRTLA registration auth

Protocol-level auth on SRTLA `REG1` / `REG2` is transport hardening, not the main product access control.

It may still be desirable in the future, but it should not be treated as the primary mechanism for:
- subscription gating
- user access control
- Relay cost protection

Those belong at the account and control-plane layers.

---

## Enforcement Rules

Hard rules:
- The plugin UI may hide or disable relay controls when unauthenticated or unentitled, but the backend remains authoritative.
- `POST /api/v1/relay/start` must enforce entitlement server-side.
- `cp_access_jwt` must never be exposed to dock JS.
- `stream_token` may be displayed to the user as operational connection info, but control-plane credentials must remain isolated in vault storage.

---

## Current Practical Boundary

Today, the intended secure path is:

1. authenticated user in plugin
2. entitlement check on relay activation
3. per-user relay stream IDs via `stream_token`
4. relay edge hardening through security group rules, CIDR allowlist, bind control, and rate limiting

This is the correct near-term model for security, compatibility, and usability.
