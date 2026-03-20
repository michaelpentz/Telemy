# v0.0.5 Plan Reviews (2026-03-18)

## Codex Review (gpt-5.4) — Backend / Go Engineering Perspective

**Overall**
The plan is directionally good, but it understates how much the control plane currently assumes a dedicated per-session relay. The biggest miss is that the control plane is not SQLite-backed today; it is PostgreSQL/`pgx`-backed in [main.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\cmd\api\main.go#L32), [store.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L14), and the existing migrations use Postgres types like `inet`, `timestamptz`, partial indexes, and `uuid` in [0001_init.sql](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\migrations\0001_init.sql#L1). Phase 4â€™s â€œscp the SQLite fileâ€ migration plan is therefore incorrect as written.

**1. Effort Estimates For Go Control Plane**
- Phase 1 is underestimated. Renaming `AWSInstanceID` is not a 2-3 hour grep. It spans API handlers, store DTOs, model fields, health validation, tests, and SQL column names across [provisioner.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\relay\provisioner.go#L5), [handlers.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\api\handlers.go#L161), [store.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L63), [types.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\model\types.go#L21), and the schema in [0001_init.sql](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\migrations\0001_init.sql#L17). Realistically: 0.5-1 day with test repair.
- EIP extraction is also more than 2-3 hours because the EIP decision changes session activation order in [runProvisionPipeline()](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\api\handlers.go#L175). If this is done carelessly, you either persist the wrong IP or leak AWS resources on partial failure. I would budget 0.5-1 day including tests.
- Hetzner shared-relay support is materially larger than 3-4 days. The provisioner itself is not the hard part; the hard part is replacing the current â€œsession owns a `relay_instances` rowâ€ model used by activation, stop, and heartbeat recording in [store.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L267), [store.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L379), and [store.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L758). Realistically: 1-1.5 weeks for backend-only shared-pool work if done cleanly.
- Phase 4 is not 0.5 day because the migration doc assumes the wrong database and omits Postgres cutover/credentials/backup/restore/rollback.

**2. Provisioner Refactor Gotchas**
- The coupling is not just `ProvisionResult.AWSInstanceID`. The session model exposes `RelayAWSInstanceID` to clients in [types.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\model\types.go#L25) and [toSessionResponse()](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\api\handlers.go#L600). That has to be renamed end-to-end.
- `RecordRelayHealth()` validates the plugin heartbeat by joining `sessions` to `relay_instances` on `ri.aws_instance_id = $7` in [store.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L761). In a shared pool, many sessions will map to one server, so â€œinstance IDâ€ is no longer a good session ownership proof.
- `ActivateProvisionedSession()` always inserts a brand-new `relay_instances` row with unique `aws_instance_id` and unique `session_id` in [store.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L276). That is fundamentally incompatible with multiple sessions sharing one relay host.
- The provision pipeline assumes the provider returns a routable host immediately and that `/health` on port `8090` is the readiness gate in [handlers.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\api\handlers.go#L292). That is fine for AWS and maybe Hetzner, but not generic BYOR.

**3. Shared Relay Pool Design**
- `relay_pool + relay_assignments` is the right direction, but it is incomplete unless you also redesign or retire `relay_instances`. Keeping both models will create contradictory sources of truth.
- `current_sessions` as a mutable counter is risky. It will drift unless assignment/release are transactional and idempotent. I would treat `relay_assignments.released_at is null` as the source of truth and derive counts in queries or maintain the counter only inside a locked transaction.
- `UNIQUE(session_id)` on `relay_assignments` is good, but you also want idempotent release semantics and probably an index on `(server_id, released_at)` for capacity checks.
- Assignment must use row-level locking, e.g. `SELECT ... FOR UPDATE SKIP LOCKED`, otherwise concurrent starts can oversubscribe the same server.
- The plan should store per-server management metadata too: SLS API endpoint, auth secret reference, and possibly private IP. A bare `host/ip` pair is not enough for safe automation.

**4. EIP Extraction / `StaticIPManager`**
- An optional `StaticIPManager` interface is reasonable, but the proposed `EnsureStaticIP(ctx, userID, instanceID)` is too AWS-shaped. The pipeline only needs a stable public address override. I would return a richer result like `ResolveRelayAddress(ctx, session, provisionResult) (publicIP string, hostname string, cleanup func, err error)` or keep it narrower as `BindPublicAddress(...)`.
- The important point is ownership of cleanup. Today EIP allocation, persistence to `users`, association, and orphan release are split between [handlers.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\api\handlers.go#L224) and [aws.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\relay\aws.go#L368). That split is what creates leak risk. I would move the whole EIP workflow behind the AWS provider, not just the EC2 SDK calls.
- For Hetzner shared relays, the interface should simply not be implemented. That part of the plan is fine.

**5. BYOR Entitlement Model**
- The current entitlement code only knows â€œallowed vs deniedâ€ and treats `starter` as denied in [GetRelayEntitlement()](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\store\store.go#L495) and [relayAccessStatus()](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\api\auth_handlers.go#L389). Adding `"byor"` is not a small tweak; it changes auth/session response shape and plugin branching.
- The existing schema forbids `free` and `selfhost` plan tiers via a CHECK constraint in [0001_init.sql](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\migrations\0001_init.sql#L12). The plan does not call out that migration.
- If BYOR can be account-optional, then the current `/auth/session`-centric UX no longer cleanly applies. The plan should explicitly choose whether the first ship requires login for BYOR. I would recommend yes for v0.0.5; offline BYOR is a separate feature.

**6. Suggestions For Hetzner Provisioner**
- Do not make the public `:8090` SLS management API the primary control-plane integration point without auth hardening. The current AWS bootstrap creates stream IDs locally with a bearer API key inside the VM in [aws.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\relay\aws.go#L153). For shared relays, you need a secure per-server management credential store and ideally private-network access.
- Make `Provision()` idempotent on `(session_id, user_id)` and have `Deprovision()` tolerate â€œalready deletedâ€ stream IDs. The current stop path can be retried.
- Persist a relay assignment record before external side effects, then mark it active only after stream-ID creation succeeds. Otherwise you will accumulate phantom capacity reservations.
- Add server draining semantics before health automation. New assignments should avoid draining servers, but existing sessions must be allowed to stop naturally.

**7. Migration Risks From EC2 To Hetzner**
- The planâ€™s database migration is the biggest operational risk because it assumes SQLite, while the code is clearly PostgreSQL-based.
- `buildManifestEntries()` in [main.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\cmd\api\main.go#L94) is still AWS AMI-oriented. If `hetzner` becomes default, that manifest path and the `/relay/manifest` route need either a provider-neutral meaning or removal.
- Region handling is AWS-shaped: defaults like `us-east-1` and `eu-west-1` come from [config.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\internal\config\config.go#L39). Hetzner regions will need a separate normalization layer, or you will leak provider-specific names through the API.
- The current shutdown behavior waits for in-flight provisioning goroutines in [main.go](E:\Code\telemyapp\telemy-v0.0.5\aegis-control-plane\cmd\api\main.go#L76). With shared relays, starts become fast, but youâ€™ll introduce always-on health monitors and assignment workers that also need lifecycle management.
- AWS fallback will bit-rot quickly unless it stays in CI. The plan says â€œkeep it in codebase,â€ but without automated coverage it will not remain a real rollback option.

**Bottom Line**
I would approve the direction with revisions. The plan should be updated to: 1) correct the database reality, 2) explicitly redesign the session/heartbeat persistence model for shared relays, 3) move EIP orchestration fully behind the AWS provider boundary, and 4) treat Hetzner shared relays as a store/model refactor first and a provisioner implementation second. Without those changes, the backend effort and migration risk are both understated.

---

# Product & Architecture Review: Telemy v0.0.5 Plan

Here is a direct, actionable review of the proposed v0.0.5 roadmap and architecture strategy.

### 1) Is the phased approach correct?
**Yes.** The ordering is logical and minimizes risk.
*   **Phase 1 (Foundation):** Essential. You cannot support BYOR or Hetzner without first decoupling the code from AWS.
*   **Phase 2 (BYOR):** High value, low risk. It validates the "provider-neutral" architecture immediately and opens the top-of-funnel (free users) without requiring the complex shared-pool logic.
*   **Phase 3 (Hetzner):** The heavy lifting. Doing this *after* BYOR ensures the plugin logic is stable before adding server-side pool management.
*   **Recommendation:** Keep the order. Do not attempt to merge Phase 2 and 3; the context switch between "client-side config" and "server-side pool management" is too high.

### 2) Is BYOR-free plus managed-paid sound for a solo dev?
**Yes.** This is the "Open Core" model applied to infrastructure.
*   **BYOR (Free):** Costs you near-zero (auth API calls). It builds an ecosystem, drives plugin installs, and serves technical users who would never pay a markup on a VPS anyway.
*   **Managed (Paid):** Targets the "I just want it to work" demographic. The value proposition is **convenience**, not the raw infrastructure.
*   **Solo Dev Benefit:** The BYOR tier effectively outsources the hardest part of the stack (reliability/infra) to the user, reducing your on-call burden for the majority of the user base.

### 3) Product risks of a free tier?
*   **Support Noise:** Users *will* blame the Telemy plugin for their misconfigured local network or cheap VPS.
    *   *Mitigation:* Aggressive "graceful degradation" in the UI. If stats fail, show a "Connection Error: Check your relay" message, not a bug report.
*   **Brand Reputation:** A user trying BYOR on a bad home connection might conclude "Telemy is bad" rather than "My connection is bad."
*   **Cannibalization:** Minimal. The type of user who manually sets up a Docker container, configures ports, and manages a VPS is not the same user who pays $8/mo for a one-click solution.

### 4) Is Hetzner the right infra choice?
**Yes.**
*   **Economics:** Bandwidth is the single biggest cost driver for video relay. Hetzner's **20TB included traffic** vs. AWS's **$0.09/GB** is the difference between a profitable business and a money pit.
*   **Performance:** Shared VPS (Pet) vs. Ephemeral EC2 (Cattle) is the correct trade-off for cost reduction. The latency difference is negligible for this use case.
*   **Constraint:** You are trading *automation* (AWS auto-scaling) for *margin* (manual/semi-automated pool management). For a solo dev, high margin is safer than high automation complexity at this scale.

### 5) Should video stabilization be v0.0.6 instead?
**Yes. Absolutely.**
*   **Scope Creep:** v0.0.5 is an *infrastructure* and *business model* update. Video stabilization is a *client-side R&D* project. Mixing them is a recipe for never shipping v0.0.5.
*   **Technical Risk:** Rust/wgpu/Compute Shaders is a completely different domain than Go/SQL/Docker.
*   **Strategic Fit:** Stabilization is a "feature," not a "foundation." Build the sustainable business model (v0.0.5) first, *then* build the killer feature (v0.0.6) to drive upgrades.

### 6) Are pricing recommendations competitive?
**Yes.**
*   **Starter ($8-10/mo):** Extremely competitive. Competitors (Speedify, commercial SRTLA services) are often $10-15+. With a ~$0.50 cost basis, your margin is >90%.
*   **Pro ($20-30/mo):** Good anchor price.
*   **Free (BYOR):** Infinite value.
*   **Observation:** The gap between $0 (Free) and $8 (Starter) is significant. Ensure the "Starter" tier offers enough friction reduction (one-click setup) to justify the leap.

### 7) Missing features or phases?
*   **Automated Billing:** The plan mentions "Entitlement" but not **Payments**. `AUTH_ENTITLEMENT_MODEL.md` suggests login is currently "operator-assisted." You cannot scale a paid tier without automated Stripe/LemonSqueezy integration. This is a critical missing piece of Phase 5.
*   **Admin Dashboard:** You need a "God View" for the shared relay pools. If a server is acting up, you need a button to "Drain & Reboot" it without digging into SQL or SSH.
*   **Abuse Prevention:** Shared IP means one bad actor (DMCA violation, DDOS target) hurts everyone on that node. You need a Terms of Service and a way to "kill switch" a specific user's stream token instantly.

### 8) Biggest risk to this plan?
**The "Noisy Neighbor" problem on shared relays.**
*   **Scenario:** User A pushes 50mbps of garbage UDP traffic or has a malformed SRT stream that spikes CPU on the `srtla_rec` process. User B on the same node sees packet loss.
*   **Impact:** Paid users experience *worse* quality than free BYOR users.
*   **Mitigation:**
    *   Strict `max_sessions` limits per node (start conservative: 10).
    *   Monitor per-container CPU/Network.
    *   Implement "Priority Tier" (Pro) that guarantees lower contention (e.g., 5 users/node) vs Starter (20 users/node).
