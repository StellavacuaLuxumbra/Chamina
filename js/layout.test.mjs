import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

import { parseLrc, applyOffset } from './lrc.js';
import { evalOrbit, cameraBasis, projectNDC } from './anim.mjs';
import { emWidth, gridPositions, layoutField, STAGES, TRAVEL_MAX } from './layout.mjs';

const DURATION = 367;
const FOV = 0.8726646;
const ASPECT = 16 / 9;
const FIT_WIDTH = 17.757;

/** The orbit the bird PV actually renders with. */
const CAMERA = {
  type: 'orbit',
  radius: 14,
  elevation: 0.09,
  target: [0, 1, 0],
  start_yaw: Math.PI / 2 - 0.3,
  speed: 0.6 / DURATION,
  movement: {
    radius_end: 12.6,
    elevation_end: 0.22,
    at: DURATION * 0.12,
    duration: DURATION * 0.76,
    ease: 'ease_in_out_cubic',
  },
};

function birdLines() {
  const src = fs.readFileSync(new URL('../scenes/bird.lrc', import.meta.url), 'utf8');
  return applyOffset(parseLrc(src)).filter((l) => l.text);
}

function birdLayout() {
  return layoutField(birdLines(), {
    camera: CAMERA,
    duration: DURATION,
    fov: FOV,
    aspect: ASPECT,
    size: 1.7,
    fitWidth: FIT_WIDTH,
    fadeIn: 0.3,
    outroAt: 318,
    titleOut: 15.34,
  });
}

/** ndc of every corner of the box around `P`. */
function corners(P, hw, hh, state, fov = FOV, aspect = ASPECT) {
  const basis = cameraBasis(state);
  return [
    [P[0] - hw, P[1] - hh, P[2]],
    [P[0] + hw, P[1] - hh, P[2]],
    [P[0] - hw, P[1] + hh, P[2]],
    [P[0] + hw, P[1] + hh, P[2]],
  ].map((c) => projectNDC(basis, c, fov, aspect));
}

function boxHalfExtents(line) {
  const em = emWidth(line.text);
  const width = Math.min(em * 1.7, FIT_WIDTH);
  return [width / 2, (width / em) * 0.55];
}

test('emWidth reproduces the widths Rust lays out', () => {
  // 17 CJK glyphs + one space; space advance measured at 0.2959 em.
  assert.ok(Math.abs(emWidth('远离那些繁华悲伤 我们从此告别了过往') - 17.2959) < 1e-9);
  assert.equal(emWidth('她的目光'), 4);
  assert.ok(
    Math.abs(emWidth('来到她身旁 谁的目光 停留在那个梦里的夜晚') - 19.5918) < 1e-9,
  );
});

test('the serpentine starts line 0 at the origin and never jumps', () => {
  const pos = gridPositions(33);
  assert.deepEqual(pos[0], [0, 0, 0]);
  assert.equal(pos.length, 33);
  let xs = 0;
  let ys = 0;
  for (let i = 1; i < pos.length; i++) {
    xs = Math.max(xs, Math.abs(pos[i][0]));
    ys = Math.max(ys, pos[i][1]);
    const dx = Math.abs(pos[i][0] - pos[i - 1][0]);
    const dy = Math.abs(pos[i][1] - pos[i - 1][1]);
    assert.ok(dx <= 30 && dy <= 15, `step ${i} is ${dx},${dy}`);
    assert.ok(dx === 0 || dy === 0, `diagonal step ${i}`);
  }
  assert.equal(xs, 60);
  assert.equal(ys, 90);
});

test('every stop settles exactly on its own cue', () => {
  const lines = birdLines();
  const { stops } = birdLayout();
  assert.equal(stops.length, 34, 'one stop per line plus the return');
  for (let i = 0; i < lines.length; i++) {
    const s = stops[i];
    assert.ok(s.duration > 0, `stop ${i} has no duration`);
    assert.ok(
      Math.abs(s.at + s.duration - lines[i].time) < 0.002,
      `stop ${i} ends at ${s.at + s.duration}, cue is ${lines[i].time}`,
    );
    assert.equal(typeof s.target[0], 'number');
    assert.ok(s.radius >= 12.6 && s.radius <= 16.6, `stop ${i} radius ${s.radius}`);
  }
  // Stops never overlap, so evalOrbit's sequential easing is unambiguous.
  for (let i = 1; i < stops.length; i++) {
    assert.ok(
      stops[i - 1].at + stops[i - 1].duration <= stops[i].at + 1e-6,
      `stop ${i} starts before stop ${i - 1} finishes`,
    );
  }
});

test('the opening card framing is untouched by the first stop', () => {
  const { stops } = birdLayout();
  const s = stops[0];
  assert.ok(s.at >= 15.34, `stop 0 starts at ${s.at}, before the card is gone`);
  const dist = Math.hypot(s.target[0], s.target[1] - 1, s.target[2]);
  assert.ok(dist < 1.6, `stop 0 swings the target ${dist} units`);
});

test('a line fits the frame with room to spare once its stop settles', () => {
  const lines = birdLines();
  const { positions, stops } = birdLayout();
  const cam = { ...CAMERA, stops };
  for (let i = 0; i < lines.length; i++) {
    const [hw, hh] = boxHalfExtents(lines[i]);
    const pts = corners(positions[i], hw, hh, evalOrbit(cam, lines[i].time));
    for (const p of pts) {
      assert.ok(p, `line ${i} has a corner behind the camera`);
      assert.ok(Math.abs(p[0]) <= 1, `line ${i} runs off the side: ${p[0].toFixed(3)}`);
      assert.ok(Math.abs(p[1]) <= 1, `line ${i} runs off the top: ${p[1].toFixed(3)}`);
    }
  }
});

test('framing actually varies from line to line', () => {
  const lines = birdLines();
  const { stops } = birdLayout();
  const seen = new Set();
  let maxX = 0;
  let spread = 0;
  for (let i = 0; i < lines.length; i++) {
    const s = stops[i];
    seen.add(`${s.radius.toFixed(2)}|${s.elevation.toFixed(3)}`);
    maxX = Math.max(maxX, Math.abs(s.target[0]));
    const base = evalOrbit(CAMERA, lines[i].time);
    spread = Math.max(spread, Math.abs(s.radius - base.radius));
  }
  assert.ok(seen.size >= 6, `only ${seen.size} distinct framings`);
  assert.ok(maxX > 40, `targets only travel ${maxX} units sideways`);
  assert.ok(spread > 2, `the dolly never moves more than ${spread}`);
});

test('each line leaves the frame only after it has fully appeared', () => {
  const lines = birdLines();
  const { positions, exits, stops } = birdLayout();
  const cam = { ...CAMERA, stops };
  assert.equal(exits.length, lines.length);
  for (let i = 0; i < lines.length; i++) {
    assert.ok(exits[i] !== null, `line ${i} never leaves the shot`);
    const reveal = lines[i].charTimes ? Math.max(...lines[i].charTimes) : lines[i].time;
    assert.ok(exits[i] >= reveal + 0.15 - 1e-6, `line ${i} fades at ${exits[i]}, reveal ends ${reveal}`);
    const [hw, hh] = boxHalfExtents(lines[i]);
    const pts = corners(positions[i], hw, hh, evalOrbit(cam, exits[i]));
    const anyFront = pts.some((p) => p);
    const anyBehind = pts.some((p) => !p);
    const on = !anyFront ? false : anyBehind ? true : pts.some((p) => Math.abs(p[0]) <= 1 && Math.abs(p[1]) <= 1);
    assert.ok(!on, `line ${i} is still on screen when it starts fading`);
  }
});

test('no line is still on screen when its successor is well under way', () => {
  const lines = birdLines();
  const { exits } = birdLayout();
  const cues = lines.map((l) => l.time);
  for (let i = 0; i < lines.length; i++) {
    if (exits[i] === null) continue;
    const next = cues[i + 1] ?? 318;
    assert.ok(
      exits[i] <= next + 4,
      `line ${i} is still up at ${exits[i]}, next cue lands ${next}`,
    );
  }
});

// The whole design leans on the outgoing line holding its place until its
// successor lights, and then on the flight being short enough that the empty
// stretch is a breath rather than a hole.
test('the screen never sits empty long before the next line lands', () => {
  const lines = birdLines();
  const { exits } = birdLayout();
  const cues = lines.map((l) => l.time);
  let worst = 0;
  for (let i = 0; i < lines.length - 1; i++) {
    if (exits[i] === null) continue;
    const blank = cues[i + 1] - exits[i];
    worst = Math.max(worst, blank);
    assert.ok(
      blank <= TRAVEL_MAX,
      `line ${i} clears at ${exits[i]}, ${blank.toFixed(2)}s of empty screen before line ${i + 1}`,
    );
  }
  assert.ok(worst > 0.2, `outgoing line lingers ${worst.toFixed(2)}s into its successor's cue`);
});

test('at most two lines ever share the frame', () => {
  const lines = birdLines();
  const { exits } = birdLayout();
  const cues = lines.map((l) => l.time);
  let peak = 0;
  for (let t = 15; t < 318; t += 0.1) {
    let n = 0;
    for (let i = 0; i < lines.length; i++) {
      const out = exits[i] === null ? 367 : exits[i];
      if (t >= cues[i] && t < out + 0.45) n++;
    }
    peak = Math.max(peak, n);
  }
  assert.ok(peak <= 2, `${peak} lines visible at once`);
});

test('the camera comes back to the title framing before the outro', () => {
  const { stops } = birdLayout();
  const back = stops[stops.length - 1];
  assert.ok(Math.abs(back.at + back.duration - 318) < 0.002, 'return lands on outro-at');
  assert.ok(Math.abs(back.target[1] - 1) < 1e-3, `target ${back.target}`);
  assert.ok(back.at > stops[stops.length - 2].at, 'return is last');
});

test('an empty song lays out to nothing', () => {
  assert.deepEqual(layoutField([], { camera: CAMERA, duration: 10 }), {
    positions: [],
    exits: [],
    stops: [],
  });
});

test('the preset cycle never exceeds the stage list', () => {
  assert.ok(STAGES.length >= 6);
  assert.equal(STAGES.length, 7);
});
