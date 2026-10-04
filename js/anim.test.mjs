// Unit tests for js/anim.mjs.
//
// Every assertion here has a twin in `src/animation.rs`. If one side drifts the
// two suites disagree and the render stops matching the exported scene.

import test from 'node:test';
import assert from 'node:assert/strict';

import {
  evalAll,
  evalAnim,
  evalBackgrounds,
  hexToRgb,
  hitEnvelope,
  jitterNoise,
  IDENTITY_Q,
} from './anim.mjs';

// ---------------------------------------------------------------------------
// legacy curves (guards the extraction of these formulas out of scene.js)
// ---------------------------------------------------------------------------

test('fall starts at height and lands', () => {
  const f = {
    type: 'fall',
    duration: 2.4,
    stagger: 0.16,
    height: 9.0,
    gravity: -18.0,
    drift: 0.0,
    start_at: 0.0,
  };
  assert.ok(Math.abs(evalAnim(f, 0, 0, 2).offset[1] - 18.0) < 1e-4);
  assert.ok(Math.abs(evalAnim(f, 5, 0, 2).offset[1]) < 1e-4);
  assert.ok(
    Math.abs(evalAnim(f, 0.1, 1, 2).offset[1] - 18.0) < 1e-3,
    'char 1 must still be at the top',
  );
});

test('slide settles at base', () => {
  const s = {
    type: 'slide',
    delta: [0, -3.5, 0],
    duration: 1.6,
    stagger: 0.05,
    ease: 'ease_out_quad',
    start_at: 0,
  };
  assert.ok(Math.abs(evalAnim(s, 0, 0, 1.5).offset[1] - -3.5 * 1.5) < 1e-4);
  assert.ok(Math.abs(evalAnim(s, 10, 0, 1.5).offset[1]) < 1e-4);
  assert.ok(Math.abs(evalAnim(s, 0.04, 1, 1.5).offset[1] - -3.5 * 1.5) < 1e-3);
});

test('fade ramps and fades out', () => {
  const f = {
    type: 'fade',
    duration: 1.0,
    stagger: 0.1,
    fade_out_at: 6.0,
    fade_out: 0.5,
    start_at: 0.0,
  };
  assert.equal(evalAnim(f, 0.0, 0, 1).alpha, 0);
  assert.ok(Math.abs(evalAnim(f, 0.5, 0, 1).alpha - 0.5) < 1e-6);
  assert.equal(evalAnim(f, 2.0, 0, 1).alpha, 1);
  assert.ok(Math.abs(evalAnim(f, 6.25, 0, 1).alpha - 0.5) < 1e-6);
  assert.equal(evalAnim(f, 7.0, 0, 1).alpha, 0);
});

test('spin is identity before delay', () => {
  const s = { type: 'spin', speed: 1.0, axis: [0, 1, 0], start_delay: 2.0 };
  assert.deepEqual(evalAnim(s, 1.0, 0, 1).rot, IDENTITY_Q);
  assert.notDeepEqual(evalAnim(s, 3.0, 0, 1).rot, IDENTITY_Q);
});

test('start_at delays the whole cue', () => {
  const fade = {
    type: 'fade',
    duration: 1.0,
    stagger: 0.2,
    start_at: 5.0,
    fade_out_at: null,
    fade_out: 1.0,
  };
  assert.equal(evalAnim(fade, 4.9, 0, 1).alpha, 0, 'hidden before the cue');
  assert.ok(Math.abs(evalAnim(fade, 5.5, 0, 1).alpha - 0.5) < 1e-6);
  assert.ok(
    Math.abs(evalAnim(fade, 5.5, 1, 1).alpha - 0.3) < 1e-6,
    'char 1 lags by exactly one stagger',
  );
  assert.equal(evalAnim(fade, 7.0, 0, 1).alpha, 1);

  const slide = {
    type: 'slide',
    delta: [-4, 0, 0],
    duration: 1.0,
    stagger: 0.0,
    ease: 'linear',
    start_at: 3.0,
  };
  assert.ok(Math.abs(evalAnim(slide, 2.0, 0, 1).offset[0] - -4) < 1e-4);
  assert.ok(Math.abs(evalAnim(slide, 4.0, 0, 1).offset[0]) < 1e-4);
});

// ---------------------------------------------------------------------------
// new: jitter
// ---------------------------------------------------------------------------

test('jitter stays parked then stays within its amplitude', () => {
  const j = {
    type: 'jitter',
    amplitude: [0.4, 0.3, 0.0],
    frequency: 5.0,
    rotation: 0.2,
    phase: 0.0,
    start_at: 4.0,
    stagger: 0.0,
    ramp: 0.5,
  };
  const parked = evalAnim(j, 3.9, 0, 2.0);
  assert.deepEqual(parked.offset, [0, 0, 0], 'no wobble before the cue');
  assert.deepEqual(parked.rot, IDENTITY_Q);

  for (let step = 0; step < 400; step++) {
    const t = 4.0 + step * 0.037;
    const st = evalAnim(j, t, 0, 2.0);
    assert.ok(Math.abs(st.offset[0]) <= 0.4 * 2.0 + 1e-4, `x out of band at ${t}`);
    assert.ok(Math.abs(st.offset[1]) <= 0.3 * 2.0 + 1e-4, `y out of band at ${t}`);
    assert.equal(st.scale, 1.0, 'jitter must not resize the glyph');
  }

  const a = evalAnim(j, 11.3, 0, 2.0).offset;
  const b = evalAnim(j, 11.3, 1, 2.0).offset;
  assert.notDeepEqual(a, b, 'each character needs its own phase');
});

test('jitter noise is bounded and axis-decorrelated', () => {
  for (let step = 0; step < 500; step++) {
    const n = jitterNoise(step * 0.11, 31.4, 1.7);
    for (const v of n) assert.ok(Math.abs(v) <= 1 + 1e-9, 'per-axis bound is 1');
  }
  const n = jitterNoise(3.3, 31.4, 0);
  assert.ok(n[0] !== n[1] && n[1] !== n[2], 'axes must not move in lockstep');
});

// ---------------------------------------------------------------------------
// new: pop
// ---------------------------------------------------------------------------

test('pop scales from zero and bounces past one', () => {
  const p = {
    type: 'pop',
    duration: 0.5,
    stagger: 0.1,
    start_at: 2.0,
    from: 0.0,
    ease: 'back_out',
  };
  assert.ok(
    Math.abs(evalAnim(p, 1.0, 0, 1).scale) < 1e-9,
    'parked at zero size',
  );
  assert.equal(evalAnim(p, 3.0, 0, 1).scale, 1.0, 'settles at rest');
  const mid = evalAnim(p, 2.15, 0, 1).scale;
  assert.ok(mid > 0 && mid < 1, `mid-ramp must be growing, got ${mid}`);

  let overshot = false;
  for (let step = 0; step < 100; step++) {
    if (evalAnim(p, 2.0 + step * 0.005, 0, 1).scale > 1) {
      overshot = true;
      break;
    }
  }
  assert.ok(overshot, 'back_out must fly past 1.0');

  assert.ok(Math.abs(evalAnim(p, 2.0, 1, 1).scale) < 1e-9, 'staggered char waits');
});

// ---------------------------------------------------------------------------
// new: wave
// ---------------------------------------------------------------------------

test('wave is flat before start and bounces per character', () => {
  const w = {
    type: 'wave',
    amplitude: 0.5,
    frequency: 1.5,
    spacing: 0.7,
    start_at: 3.0,
    ramp: 0.4,
  };
  assert.equal(evalAnim(w, 2.9, 0, 1).offset[1], 0, 'flat before the cue');
  assert.equal(evalAnim(w, 3.0, 0, 1).offset[1], 0, 'starts at zero displacement');

  let moved = false;
  for (let step = 0; step < 300; step++) {
    const t = 3.4 + step * 0.021;
    const a = evalAnim(w, t, 0, 2.0).offset[1];
    const b = evalAnim(w, t, 1, 2.0).offset[1];
    assert.ok(Math.abs(a) <= 0.5 * 2.0 + 1e-4, `amplitude bound at ${t}`);
    if (Math.abs(a) > 1e-3) {
      moved = true;
      assert.ok(Math.abs(a - b) > 1e-6, 'adjacent chars must differ');
    }
  }
  assert.ok(moved, 'the wave has to actually move');
});

// ---------------------------------------------------------------------------
// layering
// ---------------------------------------------------------------------------

test('layered state multiplies scale and alpha', () => {
  const anims = [
    { type: 'pop', duration: 1.0, stagger: 0.0, start_at: 0.0, from: 0.0, ease: 'linear' },
    { type: 'jitter', amplitude: [0.05, 0.04, 0], frequency: 5, rotation: 0, phase: 0, start_at: 0, stagger: 0, ramp: 0.3 },
    { type: 'fade', duration: 1.0, stagger: 0.0, start_at: 0.0, fade_out_at: null, fade_out: 1.0 },
  ];
  const st = evalAll(anims, 0.5, 0, 1.0);
  assert.ok(Math.abs(st.scale - 0.5) < 1e-6, 'pop drives the scale');
  assert.equal(evalAll(anims, 10.0, 0, 1.0).scale, 1.0);

  const hidden = evalAll(anims, -1, 0, 1.0);
  assert.equal(hidden.alpha, 0, 'fade still gates visibility');
});

// ---------------------------------------------------------------------------
// background timeline
// ---------------------------------------------------------------------------

const KEYS = [
  { at: 0, fade: 1.0, top: '#000000', bottom: '#000000', accent: '#000000', nebula: 0, stars: 0, drift: 1 },
  { at: 10, fade: 2.0, top: '#ffffff', bottom: '#000000', accent: '#ff0000', nebula: 0.8, stars: 0.4, drift: 3 },
];

test('an empty timeline declines so the flat colour survives', () => {
  assert.equal(evalBackgrounds([], 5), null);
  assert.equal(evalBackgrounds(null, 5), null);
});

test('before the first cut the first look owns the frame', () => {
  const s = evalBackgrounds(KEYS, 0);
  assert.deepEqual(s.top, [0, 0, 0]);
  assert.equal(s.nebula, 0);
});

test('mid-fade both palettes are part visible', () => {
  const s = evalBackgrounds(KEYS, 11);
  assert.ok(s.top[0] > 0.45 && s.top[0] < 0.55, `halfway top, got ${s.top[0]}`);
  assert.ok(s.accent[0] > 0.45 && s.accent[0] < 0.55, 'accent lerps too');
  assert.ok(s.nebula > 0.35 && s.nebula < 0.45, 'scalars lerp with the colours');
});

test('fade zero is a hard cut', () => {
  const keys = [{ ...KEYS[0] }, { ...KEYS[1], fade: 0 }];
  assert.deepEqual(evalBackgrounds(keys, 9.999).top, [0, 0, 0]);
  assert.deepEqual(evalBackgrounds(keys, 10).top, [1, 1, 1]);
});

test('hex accepts the short form and caches the long one', () => {
  assert.deepEqual(hexToRgb('#abc'), hexToRgb('#aabbcc'));
  assert.deepEqual(hexToRgb('#ff0000'), [1, 0, 0]);
});

// ---------------------------------------------------------------------------
// cue hits
// ---------------------------------------------------------------------------

test('a cue snaps on and decays to nothing', () => {
  const cues = [10];
  assert.equal(hitEnvelope(cues, 9.0, 0.4), 0, 'before the cue');
  assert.equal(hitEnvelope(cues, 10.4, 0.4), 0, 'after the ring-out');
  assert.equal(hitEnvelope(cues, 10, 0.4), 1, 'full weight the instant it lands');
  const half = hitEnvelope(cues, 10.2, 0.4);
  assert.ok(Math.abs(half - 0.25) < 1e-6, `quadratic decay, got ${half}`);
});

test('overlapping cues take the loudest, they never sum past one', () => {
  const cues = [1, 1.1, 1.2];
  let peak = 0;
  for (let t = 0; t < 2; t += 0.01) peak = Math.max(peak, hitEnvelope(cues, t, 0.5));
  assert.ok(peak <= 1.0001, `peak ${peak}`);
});

test('no cues means no hit', () => {
  assert.equal(hitEnvelope([], 3, 0.4), 0);
  assert.equal(hitEnvelope(null, 3, 0.4), 0);
});
