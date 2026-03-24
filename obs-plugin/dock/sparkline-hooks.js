import { useRef, useCallback } from "react";

const MAX_SAMPLES = 7200; // 60 min at 500ms intervals

export function useSparklineBuffer() {
  const buffersRef = useRef({});
  const seededRef = useRef({});

  const pushSample = useCallback((outputId, kbps) => {
    if (!buffersRef.current[outputId]) {
      buffersRef.current[outputId] = [];
    }
    const buf = buffersRef.current[outputId];
    buf.push({ t: Date.now(), kbps });
    if (buf.length > MAX_SAMPLES) {
      buf.splice(0, buf.length - MAX_SAMPLES);
    }
  }, []);

  // Pre-seed 60 minutes of synthetic history for an output.
  // endValue: the exact initial value the live simulation starts at, so there's no visual jump.
  const seedBuffer = useCallback((outputId, centerKbps, jitterRange, dipChance, dipAmount, endValue) => {
    if (seededRef.current[outputId]) return; // only seed once
    seededRef.current[outputId] = true;

    const now = Date.now();
    const intervalMs = 1000;
    const count = 3600; // 60 min
    const buf = [];
    let val = centerKbps;
    const target = endValue != null ? endValue : centerKbps;

    for (let i = 0; i < count; i++) {
      const t = now - (count - i) * intervalMs;
      const progress = i / count;

      // In the last 5% of samples, converge toward endValue
      if (progress > 0.95) {
        const blend = (progress - 0.95) / 0.05; // 0..1 over last 5%
        const drift = (Math.random() - 0.5) * jitterRange * (1 - blend);
        val = val + (target - val) * 0.1 + drift;
      } else if (Math.random() < dipChance) {
        val = Math.max(centerKbps * 0.65, val - dipAmount * 0.7);
      } else {
        const drift = (Math.random() - 0.5) * jitterRange;
        const pull = (centerKbps - val) * 0.15;
        val = Math.max(centerKbps * 0.65, Math.min(centerKbps * 1.2, val + drift + pull));
      }
      buf.push({ t, kbps: Math.round(val) });
    }

    buffersRef.current[outputId] = buf;
  }, []);

  const getSamples = useCallback((outputId, windowMs) => {
    const buf = buffersRef.current[outputId];
    if (!buf || buf.length === 0) return [];
    const cutoff = Date.now() - windowMs;
    let start = 0;
    for (let i = buf.length - 1; i >= 0; i--) {
      if (buf[i].t < cutoff) { start = i + 1; break; }
    }
    return buf.slice(start);
  }, []);

  const downsample = useCallback((samples, maxPoints) => {
    if (samples.length <= maxPoints) return samples;
    const step = samples.length / maxPoints;
    const result = [];
    for (let i = 0; i < maxPoints; i++) {
      const idx = Math.min(Math.floor(i * step), samples.length - 1);
      result.push(samples[idx]);
    }
    if (result.length > 0 && result[result.length - 1] !== samples[samples.length - 1]) {
      result[result.length - 1] = samples[samples.length - 1];
    }
    return result;
  }, []);

  return { pushSample, seedBuffer, getSamples, downsample };
}
