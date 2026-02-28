# Telemy v0.0.3 Overhaul Prep

This workspace is the dedicated staging area for the `v0.0.3` overhaul.
Base copied from `telemy-v0.0.2` on 2026-02-21.

## Objectives

- Enable large structural refactors without destabilizing `v0.0.2`.
- Keep release-quality gates visible throughout the rewrite.
- Track migration decisions and compatibility impacts explicitly.

## Phase Checklist

## 1) Baseline Freeze

- [x] Create isolated `telemy-v0.0.3` workspace
- [x] Remove copied build artifacts (`obs-telemetry-bridge/target`)
- [x] Bump crate version baseline to `0.0.3`
- [x] Tag/record `v0.0.2` baseline commit hash in release notes

## 2) Architecture Changes

- [x] Define target module boundaries (`app`, `metrics`, `server`, `security`, `startup`)
- [x] Identify breaking API/CLI/config changes
- [x] Create migration map from old module responsibilities to new ownership
- [x] Establish v1 contracts in `docs/` (API, DB schema, state machine, IPC)

## 3) Data + Config Migration

- [ ] Version and validate `config.toml` format updates (Rust app path)
- [ ] Add migration path for persisted runtime data and vault usage (Rust app path)
- [ ] Add backward compatibility behavior (or explicit failure messaging)

## 4) Quality Gates

- [x] Go backend unit tests for API/store/relay behavior
- [ ] Rust-side unit tests for new behavior (config, transforms, auth)
- [ ] `cargo fmt`
- [ ] `cargo clippy -- -D warnings`
- [ ] `cargo test`
- [ ] `cargo build --release`

## 5) Release Readiness

- [ ] Update installer assets/scripts if structure changes
- [ ] Refresh Grafana dashboard template if metric schema changes
- [ ] Document upgrade notes (`v0.0.2 -> v0.0.3`)
- [ ] Add metrics endpoint and alertable SLO metrics for control plane
- [ ] Decide single-process vs split `cmd/api` + `cmd/jobs` runtime

## Current Focus

1. Complete observability pass (`/metrics`, counters, histograms).
2. Finalize AWS retry policy details (jitter, budget caps, error taxonomy).
3. Harden deployment model and release checklist for mixed Rust + Go components.

## Working Rules

- Make incremental commits by subsystem.
- Prefer feature flags for partial integrations during refactor.
- Do not commit secrets or populated runtime credentials.
