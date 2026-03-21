# VPS Relay Server Comparison

> Decision date: 2026-03-20
> Decision: Advin Servers (chosen)

## Requirements

- High bandwidth per dollar (streaming relay = bandwidth-heavy, not compute-heavy)
- UDP port flexibility (SRT/SRTLA requires UDP 4000–5000 open)
- Low per-GB cost or unmetered at a reasonable price point
- IPv4 included
- KVM or dedicated (no OpenVZ/container VPS — Docker nested containers)

---

## Comparison

| Provider | Plan | Price/mo | CPU | RAM | Storage | Bandwidth | Per-TB cost |
|----------|------|----------|-----|-----|---------|-----------|-------------|
| **Advin Servers** ✅ | KVM Standard XS | **$8** | 4 vCPU | 8 GB | 64 GB NVMe | **32 TB** | **$0.25/TB** |
| Hetzner | CX22 | €4.49 (~$5) | 2 vCPU (shared) | 4 GB | 40 GB SSD | 20 TB | ~$0.25/TB |
| Hetzner | CX32 | €8.49 (~$9) | 4 vCPU (shared) | 8 GB | 80 GB SSD | 20 TB | ~$0.45/TB |
| DigitalOcean | Basic 4GB | $24 | 2 vCPU | 4 GB | 80 GB SSD | 4 TB | $6/TB |
| Vultr | High Performance 4GB | $24 | 2 vCPU | 4 GB | 100 GB NVMe | 3 TB | $8/TB |
| AWS EC2 | t3.small (us-west-2) | ~$15 + BW | 2 vCPU | 2 GB | EBS | 0 (pay per GB) | ~$90/TB |

---

## Analysis

**Advin Servers** — best bandwidth-per-dollar for this use case:
- 32 TB included at $8/mo is exceptional for a relay server
- SRT/SRTLA traffic is ~500 Kbps–5 Mbps per stream, so 32 TB handles hundreds of stream-hours
- KVM hypervisor: Docker works correctly, no nested container issues
- Kansas City location (kc1) good for US-central coverage

**Hetzner** — strong runner-up, especially for EU coverage:
- CX22 at €4.49 is the cheapest option but only 20 TB and shared CPU
- Excellent for EU relay nodes (Frankfurt/Helsinki)
- Hetzner was the original Phase 3 target before Advin was evaluated
- Still a good choice for future EU nodes

**DigitalOcean / Vultr** — eliminated:
- 4 TB included bandwidth at $24 is 8× worse bandwidth per dollar than Advin
- Per-GB overage is expensive

**AWS EC2** — eliminated for relay nodes:
- Egress bandwidth charges ($0.09/GB = ~$90/TB) make it untenable for high-volume relay
- t3.small + ~5TB/mo streaming = $15 instance + $450 bandwidth = not viable at scale
- Was used for ephemeral relay during early development due to control plane co-location

---

## Decision

**Advin Servers** for relay pool nodes. Initial deployment: `kc1` (Kansas City, `kc1.relay.telemyapp.com`).

Future expansion:
- EU node: Hetzner CX22 (Frankfurt) — best EU coverage at lowest cost
- US-East: Advin or Hetzner depending on availability

The `PoolProvisioner` (Phase 3) is fully provider-agnostic — any always-on server running the srtla-receiver Docker stack can be registered in the `relay_pool` table regardless of hosting provider.
