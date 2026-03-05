import { useState, useEffect, useRef } from "react";

// =============================================================================
// HOOKS
// =============================================================================

export function useAnimatedValue(target, duration = 800) {
  const [value, setValue] = useState(target);
  const ref = useRef(target);
  useEffect(() => {
    const start = ref.current;
    const diff = target - start;
    if (Math.abs(diff) < 0.5) { ref.current = target; setValue(target); return; }
    const startTime = performance.now();
    let raf;
    const step = (now) => {
      const elapsed = now - startTime;
      const progress = Math.min(elapsed / duration, 1);
      const eased = 1 - Math.pow(1 - progress, 3);
      setValue(start + diff * eased);
      if (progress < 1) raf = requestAnimationFrame(step);
      else ref.current = target;
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, [target, duration]);
  return value;
}

export function useRollingMaxBitrate(outputItems) {
  const maxMapRef = useRef({});
  const resultRef = useRef({ maxMap: {}, sectionMax: 0 });
  // Compute inline during render — intentional ref mutation for rolling tracker.
  // No setState to avoid re-render loops when outputItems is a fresh array each render.
  const maxMap = maxMapRef.current;
  let sectionMax = 0;
  if (Array.isArray(outputItems)) {
    outputItems.forEach(item => {
      const key = item.id || item.name || item.platform;
      if (key && item.kbps > 0) {
        const prev = maxMap[key] || 0;
        const decayed = prev > 0 ? prev * 0.998 : 0;
        maxMap[key] = Math.max(decayed, item.kbps);
      }
      if (key && maxMap[key] > sectionMax) sectionMax = maxMap[key];
    });
  }
  resultRef.current = { maxMap, sectionMax };
  return resultRef.current;
}

export function useDockCompactMode(containerRef) {
  const [mode, setMode] = useState("regular");

  useEffect(() => {
    const el = containerRef.current;
    if (!el || typeof ResizeObserver === "undefined") return;

    const applyMode = (width) => {
      if (width <= 280) {
        setMode("ultra");
      } else if (width <= 340) {
        setMode("compact");
      } else {
        setMode("regular");
      }
    };

    applyMode(el.clientWidth || 0);
    const ro = new ResizeObserver((entries) => {
      const width = entries?.[0]?.contentRect?.width || el.clientWidth || 0;
      applyMode(width);
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, [containerRef]);

  return mode;
}
