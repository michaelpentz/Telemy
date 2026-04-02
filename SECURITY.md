# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in any Telemyapp project, please report it responsibly.

**Email:** support@telemyapp.com

Please include:
- Description of the vulnerability
- Steps to reproduce
- Affected component (plugin, relay, control plane, website)
- Impact assessment if known

## Response Timeline

- **Acknowledgment:** Within 48 hours
- **Triage & severity assessment:** Within 5 business days
- **Fix or mitigation:** Based on severity (critical: ASAP, high: 7 days, medium: 30 days)

## Scope

| In Scope | Out of Scope |
|----------|-------------|
| OBS plugin (DLL) | Third-party OBS plugins |
| Control plane API | Upstream SRT/SRTla protocol bugs |
| telemyapp.com web app | Social engineering attacks |
| Relay infrastructure | Denial of service |
| Authentication & session management | Issues already reported |

## Disclosure Policy

- We ask that you do not publicly disclose the vulnerability until we have released a fix or 90 days have passed, whichever comes first.
- We will credit reporters in release notes unless anonymity is requested.

## Security Practices

- Encrypted credential storage (DPAPI)
- JWT-based authentication with enforced minimum key length
- Per-IP rate limiting on all auth endpoints
- CSRF protection via `X-Requested-With` headers
- HttpOnly session cookies (no localStorage tokens)
- Webhook idempotency and message staleness validation
- Admin mutations require password re-authentication with audit logging
