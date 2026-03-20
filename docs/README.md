# Telemy Documentation Index (v0.0.5)

This directory contains the authoritative specifications, architecture diagrams, and operational guides for **Telemy v0.0.5**.

## Current Architecture

v0.0.5 continues the all-native C++ OBS plugin architecture, originally introduced in v0.0.4 as a replacement for the multi-process Rust/C++ hybrid. Single DLL, no IPC layer: **`aegis-obs-plugin.dll`**.

- **[ARCHITECTURE.md](ARCHITECTURE.md)**: Detailed mapping of the single-DLL plugin, including metrics collection, relay communication, and dock hosting.
- **[API_SPEC_v1.md](API_SPEC_v1.md)**: Cloud API contracts for relay lifecycle, session management, and usage metering.
- **[AUTH_ENTITLEMENT_MODEL.md](AUTH_ENTITLEMENT_MODEL.md)**: Recommended user login, entitlement enforcement, plugin auth, and per-user stream security model for the paid relay feature.
- **[STATE_MACHINE_v1.md](STATE_MACHINE_v1.md)**: Authoritative runtime state machine and scene decision rules for the native plugin.
- **[DB_SCHEMA_v1.md](DB_SCHEMA_v1.md)**: PostgreSQL schema supporting the v0.0.5 control plane.
- **[RELAY_DEPLOYMENT.md](RELAY_DEPLOYMENT.md)**: Guide for deploying and managing the AWS EC2 relay stack using `srtla-receiver`.

## Operational & QA Guides

- **[QA_CHECKLIST_RELAY_TELEMETRY.md](QA_CHECKLIST_RELAY_TELEMETRY.md)**: Validation steps for bonded relay and telemetry data flow.
- **[OPERATIONS_METRICS.md](OPERATIONS_METRICS.md)**: Monitoring and alerting targets for the control plane and relay fleet.

## Historical Notes

v0.0.3-era bridge / IPC documents and ad hoc review notes were removed from this directory and archived outside the `telemy-v0.0.5` repo tree so they do not ship with the current versioned docs.

---
*Last Updated: 2026-03-08 (Sync after hardening pass)*
