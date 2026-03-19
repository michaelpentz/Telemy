# Project Aegis — Future Exploration: Real-Time Video Stabilization

**Status:** Deferred — explore after stable prototype  
**Phase:** 3+ (after local Phase 1 and cloud Phase 2)  
**Priority:** Enhancement / Differentiator  
**Added:** 2026-03-18

---

## 1. Summary

Integrate an optional real-time video stabilization filter into the Aegis OBS plugin for IRL streaming. This would give run-and-gun streamers software-based stabilization on their incoming video feed with an adjustable crop factor — no physical gimbal or in-camera EIS required. The feature would be local-only, opt-in, and completely decoupled from core failover logic.

---

## 2. Market Context

### Why This Matters for IRL Streaming

IRL streamers are Aegis's core audience, and shaky footage is one of the most common quality complaints in run-and-gun content. Current solutions each have gaps: physical gimbals (DJI OM series) are bulky and limit how you hold the phone, in-camera EIS (GoPro HyperSmooth, iPhone Action Mode) adds crop and isn't available on all devices, and in-app stabilization varies wildly in quality.

A software stabilizer running on the receiving PC — where the hardware is typically much more powerful than the phone — fills a real gap. It could process the SRT/SRTLA feed after it arrives and before OBS encodes it for the stream output.

### Why LiveVisionKit (the Inspiration) Stayed Niche

1. **Audience mismatch:** IRL streamers who need stabilization most often don't have powerful local GPUs (they're not gamers). Desktop streamers with powerful GPUs don't need stabilization (they're sitting at a desk).
2. **Existing hardware solutions** cover most use cases before OBS is even involved.
3. **Solo maintainer burnout** — hundreds of R&D hours per feature, unsustainable for one developer.
4. **Audio sync friction** — frame buffering requires manual audio delay, which is a UX pain point.

### Where Aegis Has an Advantage

- **Built-in audience:** Aegis specifically targets IRL streamers — the people who benefit most.
- **Bundled value:** Stabilization as part of a complete IRL toolkit (failover + relay + chatbot + stabilization) is more compelling than a standalone filter.
- **Audio sync opportunity:** The Rust core could handle audio delay compensation automatically.
- **Hardware gating:** A built-in scanner can restrict the feature to capable hardware, avoiding bad experiences.

---

## 3. Reference: LiveVisionKit (LVK) by Crowsinc

- **Repository:** `github.com/Crowsinc/LiveVisionKit`
- **Status:** Archived April 21, 2024 (read-only, indefinite pause)
- **Last release:** v1.2.2 (February 2023)
- **License:** Review before any code reference
- **Developer stance:** Open to collaboration/forking (see repo README and OBS Forums)

### What LVK Got Right

- GPU-accelerated stabilization running as a native OBS filter
- Configurable smoothing radius and crop percentage
- Multiple motion models (Affine, Homography, Dynamic auto-select)
- Suppression modes that auto-disable when content is unsuitable for stabilization
- AMD FSR 1.0 integration for upscaling after crop

### Where LVK Struggled

- OpenCL driver compatibility issues across different GPU/driver configurations
- HDR content incompatibility (filters auto-disable under HDR)
- Limited hardware testing coverage
- Audio desync from frame buffering (users must manually delay audio)
- Occasional crashes on certain GPU/driver combos (RTX 3070 + older drivers, various Linux distros)

---

## 4. Proposed Architecture

### Do NOT Fork LiveVisionKit

LVK is built on OpenCV + OpenCL in C++. Forking introduces a large C++ dependency, OpenCL driver headaches, and a codebase with different design conventions than Aegis. Use LVK as **algorithm and UX reference only** — build fresh in Rust.

### Implementation: Rust with wgpu or Vulkan Compute Shaders

- Stays within the existing `aegis-core.exe` Rust process
- `wgpu` provides cross-platform GPU compute abstraction over Vulkan/DX12/Metal
- Alternatively, raw Vulkan compute via `ash` crate for maximum control
- Avoids OpenCL driver compatibility issues entirely
- Consider `opencv-rust` bindings only if wgpu proves insufficient for motion estimation

### Processing Pipeline

```
Incoming frame (from SRT/SRTLA source or OBS source)
    |
    v
Motion Estimation (compute shader — feature detection + optical flow)
    |
    v
Smoothing Buffer (N frames, configurable radius)
    |
    v
Corrective Transform (compute shader — warp/rotate/translate)
    |
    v
Crop & Scale (adjustable crop %, optional FSR/sharpening upscale)
    |
    v
Output frame -> OBS render pipeline -> NVENC/AMF encode
```

### Decoupling Principle

The stabilizer must be a completely independent optional filter with zero interaction with the failover state machine, scene switching, or bitrate monitoring. Enable/disable must not affect any Aegis core functionality.

---

## 5. Hardware Requirements & Resource Usage

### What Gets Taxed

| Resource | Usage Level | Notes |
|----------|-------------|-------|
| GPU Compute Cores (CUDA/Shader) | **Primary** | Motion estimation + frame transforms |
| VRAM | ~200-400MB at 1080p | Smoothing buffer + processing intermediates |
| GPU Memory Bandwidth | Moderate | Frame data shuffling to/from GPU |
| CPU | Minimal | Orchestration and scheduling only |
| NVENC/AMF (HW encoder) | **Not used** by stabilizer | Encoding runs in parallel on dedicated silicon |

Key insight: stabilization uses **compute cores**, not the dedicated hardware encoder blocks. This means stabilization and encoding can run simultaneously without directly competing for the same silicon. They share VRAM and memory bandwidth, which is where contention could occur.

### Compatibility Matrix

| Scenario | Feasibility | Notes |
|----------|-------------|-------|
| Dedicated encoding PC (RTX 2060+) | **Excellent** | Compute cores mostly idle; NVENC handles encoding separately |
| Dual-output H/V encoding | **Good** | Stabilize once, crop twice, two NVENC sessions in parallel |
| Single PC gaming + streaming (RTX 4080/5080) | **Possible** | Game competes for compute cores; needs real-time utilization monitoring |
| Low-end GPU (GTX 1060 or below) | **Not recommended** | Insufficient compute headroom |

### Hardware Scanner Implementation

The Aegis core should gate stabilization behind a hardware capability check:

- **GPU generation:** Minimum RTX 2000 series (NVIDIA) / RX 5000 series (AMD)
- **Available VRAM:** Minimum ~1GB free after OBS and other processes
- **Vulkan/Compute support:** Verify capability at runtime
- **Real-time utilization gate:** If GPU compute utilization exceeds ~75%, warn or auto-disable
- **Dashboard indicator:** Traffic light display (green / yellow / red) for stabilization readiness

GPU stats queried via NVML (`nvml-wrapper` crate for NVIDIA) or equivalent AMD ADL/ROCm APIs.

---

## 6. Crop Factor & UX

Crop is inherent to all digital stabilization — the stabilizer shifts the visible frame within a larger buffer zone at the edges. More crop means more room to correct for larger movements, but less visible field of view and resolution.

### Crop Budget Examples (1080p input)

| Crop % | Buffer (per edge) | Effective Resolution | After Upscale |
|--------|-------------------|---------------------|---------------|
| 3% | ~29px | 1862 x 1048 | Upscaled to 1920x1080 |
| 5% | ~48px | 1824 x 1026 | Upscaled to 1920x1080 |
| 10% | ~96px | 1728 x 972 | Upscaled to 1920x1080 |

### UX Recommendations

- Expose crop % as a slider in the Aegis dashboard (range: 1-15%)
- Sensible defaults: 3-5% for handheld, 8-10% for walking/running
- Consider an AMD FSR 1.0 or similar sharpening pass post-crop (LVK did this effectively)
- Shooting at higher source resolution (1440p, 4K) and outputting 1080p provides natural crop headroom — note this in documentation
- LVK's "Suppression Mode" concept is worth replicating: auto-disable stabilization when frame content is unsuitable (e.g., rapid intentional panning)

---

## 7. Rejected Approach: Cloud-Hosted Stabilization

Evaluated and rejected for three reasons:

**Latency:** Round-trip frame shuttle adds 55-135ms per frame minimum (encode, upload, GPU process, download, decode). Unacceptable for real-time streaming.

**Bandwidth:** Raw frame transport requires 6-15 MB/s at 30fps, on top of the stream the user is already sending. IRL streamers on mobile connections can't support this — and they're the ones who need stabilization most.

**Cost:** GPU cloud instances ($0.30-1.20/hr per session) dramatically exceed the margins of a time-bank pricing model. A local GPU does this for free.

**Relay-side processing** was also evaluated (frames already pass through the SRTLA receiver). Rejected because it would require GPU-equipped instances, breaking the ARM64 EC2 ephemeral architecture and multiplying infrastructure costs.

**Verdict: Local-only, always.**

---

## 8. Open Questions & Action Items

### Questions to Resolve at Implementation Time

- **Motion estimation algorithm:** Optical flow (ORB/AKAZE features) vs. phase correlation vs. block matching — which gives the best quality/perf tradeoff in wgpu compute shaders?
- **Audio sync automation:** Can the Rust core detect and compensate for frame buffer delay automatically, or will users need manual adjustment?
- **OBS integration point:** Register as an OBS filter (per-source) or handle entirely within aegis-core before frames reach OBS?
- **Gyroscope data:** Some phones transmit gyro metadata alongside video — could this enable hybrid stabilization when available via the SRT feed?
- **Interaction with OBS transforms:** How does this compose with OBS scene transforms, crop/pad filters, and built-in scaling?

### Action Items (When Ready to Explore)

- [ ] Review LVK source for algorithm reference (motion models, suppression logic, crop handling)
- [ ] Prototype minimal wgpu compute shader for frame-to-frame motion estimation
- [ ] Benchmark VRAM and compute utilization across target GPU generations (RTX 2060, 3060, 4070, 5080)
- [ ] Design dashboard UI controls (toggle, crop slider, smoothing slider, HW readiness indicator)
- [ ] Investigate automatic audio delay compensation in aegis-core
- [ ] Evaluate `opencv-rust` vs pure wgpu for motion estimation quality
- [ ] Review LVK license for any usage/reference restrictions
