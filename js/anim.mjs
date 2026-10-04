// Pure animation arithmetic shared by the browser renderer (js/scene.js) and
// the node test suite (js/anim.test.mjs).
//
// This mirrors src/animation.rs. One formula, two implementations: whenever a
// curve changes, change it here *and* there, and keep both test files asserting
// the same properties.

// ---------------------------------------------------------------------------
// easing + scalar helpers
// ---------------------------------------------------------------------------

export const clamp01 = (x) => (x < 0 ? 0 : x > 1 ? 1 : x);

/** Same shape as Rust's `smoothstep` helper in animation.rs. */
export const smoothstep = (x) => {
  const v = clamp01(x);
  return v * v * (3 - 2 * v);
};

export const EASE = {
  linear: (x) => x,
  smooth: (x) => x * x * (3 - 2 * x),
  ease_in_quad: (x) => x * x,
  ease_out_quad: (x) => 1 - (1 - x) * (1 - x),
  ease_in_out_cubic: (x) =>
    x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2,
  back_out: (x) => {
    const C1 = 1.70158;
    const C3 = C1 + 1;
    return 1 + C3 * Math.pow(x - 1, 3) + C1 * Math.pow(x - 1, 2);
  },
};

export function easeApply(name, x) {
  const f = EASE[name] || EASE.smooth;
  return f(clamp01(x));
}

// ---------------------------------------------------------------------------
// quaternions (mirrors chamina::math)
// ---------------------------------------------------------------------------

export const IDENTITY_Q = [0, 0, 0, 1];

/** Hamilton product: (a * b) applies b first, then a. Quats are [x, y, z, w]. */
export function quatMul(a, b) {
  return [
    a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  ];
}

export function axisAngle(axis, angle) {
  const len = Math.hypot(axis[0], axis[1], axis[2]) || 1;
  const a = [axis[0] / len, axis[1] / len, axis[2] / len];
  const h = angle * 0.5;
  const s = Math.sin(h);
  return [a[0] * s, a[1] * s, a[2] * s, Math.cos(h)];
}

// ---------------------------------------------------------------------------
// built-in curves
// ---------------------------------------------------------------------------

export function fallOffset(f, u) {
  const x = clamp01(u);
  const t = f.duration * x;
  const v0 = -(f.height + 0.5 * f.gravity * f.duration * f.duration) / f.duration;
  return f.height + v0 * t + 0.5 * f.gravity * t * t;
}

export function fadeFactor(f, t, delay) {
  const t0 = delay;
  let v = 1;
  if (t < t0) {
    v = 0;
  } else if (t < t0 + f.duration) {
    v = (t - t0) / Math.max(f.duration, 1e-6);
  }
  if (f.fade_out_at !== null && f.fade_out_at !== undefined) {
    const fo = f.fade_out_at;
    const fin = fo + f.fade_out;
    if (t > fo) {
      v *= clamp01(1 - (t - fo) / Math.max(f.fade_out, 1e-6));
      if (t >= fin) v = 0;
    }
  }
  return clamp01(v);
}

// ---------------------------------------------------------------------------
// jitter noise (mirrors `jitter_noise` in animation.rs)
// ---------------------------------------------------------------------------

// Two detuned sines per axis. Sines rather than a hash on purpose: Math.sin and
// f32::sin agree to rounding, while a seeded hash would diverge between the two
// implementations after the first frame.
const HARMONIC = [1.618034, Math.SQRT2, 1.7320508];
const AXIS_PHASE = [0.0, 2.0943951, 4.1887902];
/** Golden angle between neighbouring characters. */
const CHAR_PHASE = 2.3999632;

export function jitterNoise(t, w, p) {
  const out = [0, 0, 0];
  for (let i = 0; i < 3; i++) {
    const ph = p + AXIS_PHASE[i];
    out[i] = 0.5 * Math.sin(w * t + ph) + 0.5 * Math.sin(w * HARMONIC[i] * t + ph * 1.7);
  }
  return out;
}

// ---------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------

export const IDENTITY_STATE = () => ({
  offset: [0, 0, 0],
  rot: IDENTITY_Q,
  alpha: 1,
  scale: 1,
});

export function evalAnim(a, t, idx, size) {
  const st = IDENTITY_STATE();
  switch (a.type) {
    case 'fall': {
      const delay = (a.start_at ?? 0) + idx * a.stagger;
      const u = (t - delay) / Math.max(a.duration, 1e-6);
      st.offset[1] += fallOffset(a, u) * size;
      if (t > delay) {
        const driftT = Math.min(t - delay, a.duration);
        st.offset[0] += a.drift * driftT * size;
      }
      break;
    }
    case 'fade': {
      st.alpha = fadeFactor(a, t, (a.start_at ?? 0) + idx * a.stagger);
      break;
    }
    case 'spin': {
      st.rot =
        t < a.start_delay
          ? IDENTITY_Q
          : axisAngle(a.axis, a.speed * (t - a.start_delay));
      break;
    }
    case 'slide': {
      const delay = (a.start_at ?? 0) + idx * a.stagger;
      const u = (t - delay) / Math.max(a.duration, 1e-6);
      const k = (1 - easeApply(a.ease, u)) * size;
      st.offset[0] += a.delta[0] * k;
      st.offset[1] += a.delta[1] * k;
      st.offset[2] += a.delta[2] * k;
      break;
    }
    case 'jitter': {
      const delay = (a.start_at ?? 0) + idx * a.stagger;
      if (t >= delay) {
        const p = (a.phase ?? 0) + idx * CHAR_PHASE;
        const n = jitterNoise(t, 2 * Math.PI * Math.max(a.frequency, 0), p);
        const env = smoothstep((t - delay) / Math.max(a.ramp ?? 0.3, 1e-6));
        const amp = a.amplitude || [0.05, 0.04, 0];
        const k = size * env;
        st.offset[0] += n[0] * amp[0] * k;
        st.offset[1] += n[1] * amp[1] * k;
        st.offset[2] += n[2] * (amp[2] ?? 0) * k;
        if (a.rotation) {
          const r = a.rotation * env;
          st.rot = quatMul(axisAngle([0, 0, 1], n[0] * r), axisAngle([0, 1, 0], n[1] * r * 0.5));
        }
      }
      break;
    }
    case 'pop': {
      const delay = (a.start_at ?? 0) + idx * a.stagger;
      const u = clamp01((t - delay) / Math.max(a.duration, 1e-6));
      const from = a.from ?? 0;
      st.scale = from + (1 - from) * easeApply(a.ease ?? 'back_out', u);
      break;
    }
    case 'wave': {
      const at = a.start_at ?? 0;
      if (t >= at) {
        const env = smoothstep((t - at) / Math.max(a.ramp ?? 0.4, 1e-6));
        const phase = 2 * Math.PI * a.frequency * (t - at) - idx * (a.spacing ?? 0.55);
        st.offset[1] += a.amplitude * env * Math.sin(phase) * size;
      }
      break;
    }
    default:
      break;
  }
  return st;
}

export function evalAll(anims, t, idx, size) {
  const out = IDENTITY_STATE();
  for (const a of anims || []) {
    const st = evalAnim(a, t, idx, size);
    out.offset[0] += st.offset[0];
    out.offset[1] += st.offset[1];
    out.offset[2] += st.offset[2];
    out.rot = quatMul(out.rot, st.rot);
    out.alpha *= st.alpha;
    out.scale *= st.scale;
  }
  return out;
}

// ---------------------------------------------------------------------------
// background timeline + cue hits (mirror of src/scene.rs data, JS-only maths)
// ---------------------------------------------------------------------------

const HEX_CACHE = new Map();

/** `#rrggbb` / `#rgb` -> `[r, g, b]` in 0..1 (sRGB, as authored). */
export function hexToRgb(hex) {
  if (Array.isArray(hex)) return hex;
  const hit = HEX_CACHE.get(hex);
  if (hit) return hit;
  let h = String(hex || '#000000').replace(/^#/, '');
  if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
  const n = parseInt(h.slice(0, 6), 16);
  const out = [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
  HEX_CACHE.set(hex, out);
  return out;
}

const mix = (a, b, k) => [
  a[0] + (b[0] - a[0]) * k,
  a[1] + (b[1] - a[1]) * k,
  a[2] + (b[2] - a[2]) * k,
];

/**
 * Evaluate the background timeline at `t`.
 *
 * Entries are read in list order; the active look is the last one whose `at`
 * has passed, and it crossfades from its predecessor over `fade` seconds with
 * the same smoothstep the rest of the crate uses. `fade: 0` is a hard cut.
 * Returns `null` for an empty list so the caller can keep the flat colour.
 */
export function evalBackgrounds(keys, t) {
  if (!keys || keys.length === 0) return null;
  let i = 0;
  for (let k = 0; k < keys.length; k++) {
    if ((keys[k].at ?? 0) <= t) i = k;
  }
  const cur = keys[i];
  const state = {
    // Copies: `hexToRgb` hands back a memoised array and the caller owns
    // whatever this returns.
    top: [...hexToRgb(cur.top)],
    bottom: [...hexToRgb(cur.bottom)],
    accent: [...hexToRgb(cur.accent)],
    nebula: cur.nebula ?? 0.5,
    stars: cur.stars ?? 0.6,
    drift: cur.drift ?? 1,
  };
  if (i === 0) return state;

  const prev = keys[i - 1];
  const fade = Math.max(cur.fade ?? 0, 0);
  if (fade <= 0) return state;
  const k = smoothstep((t - (cur.at ?? 0)) / fade);
  return {
    top: mix(hexToRgb(prev.top), state.top, k),
    bottom: mix(hexToRgb(prev.bottom), state.bottom, k),
    accent: mix(hexToRgb(prev.accent), state.accent, k),
    nebula: (prev.nebula ?? 0.5) + (state.nebula - (prev.nebula ?? 0.5)) * k,
    stars: (prev.stars ?? 0.6) + (state.stars - (prev.stars ?? 0.6)) * k,
    drift: (prev.drift ?? 1) + (state.drift - (prev.drift ?? 1)) * k,
  };
}

/**
 * Summed envelope of every cue hit at `t`: snaps to 1 the instant a cue lands
 * and decays to 0 over `dur` seconds. Drives the flash, the chromatic split
 * and the camera punch, so every line lands with a bit of weight.
 */
export function hitEnvelope(cues, t, dur) {
  if (!cues || cues.length === 0) return 0;
  const d = Math.max(dur ?? 0.35, 1e-6);
  let best = 0;
  for (const c of cues) {
    const u = (t - c) / d;
    if (u < 0 || u > 1) continue;
    const v = (1 - u) * (1 - u);
    if (v > best) best = v;
  }
  return best;
}
