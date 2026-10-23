import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

import { parseLrc, applyOffset, wordGroups, groupCharTimes } from './lrc.js';
import { evalOrbit, cameraBasis, projectNDC } from './anim.mjs';
import {
  emWidth,
  gridPositions,
  layoutField,
  assignChoreo,
  signatureOf,
  STAGES,
  TRAVEL_MAX,
} from './layout.mjs';

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

function birdGroupedLayout() {
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
    groups: 'auto',
  });
}

/** The per-word phrase of every line, in the order layoutField used them. */
function birdGroups() {
  return birdLines().map((l, i) => wordGroups(l.text, l.charTimes, i));
}

/**
 * Split the stop list into per-line phrases. A stop belongs to line i when it
 * ends on or after line i's cue and strictly before the next line's — so the
 * flight in belongs to the line it lands on, and the return to the title
 * framing (which starts after the last cue) belongs to nobody.
 */
function phraseStops(lines, stops) {
  const ends = stops.map((s) => s.at + s.duration);
  return lines.map((l, i) => {
    const next = i + 1 < lines.length ? lines[i + 1].time : stops[stops.length - 1].at;
    return stops.filter((s, k) => ends[k] >= l.time - 1e-6 && ends[k] < next - 1e-6);
  });
}

/** ndc of every corner of the box around `P`, with the camera's roll applied. */
function corners(P, hw, hh, state, fov = FOV, aspect = ASPECT) {
  const basis = cameraBasis(state);
  const roll = state.roll || 0;
  const c = Math.cos(-roll);
  const s = Math.sin(-roll);
  return [
    [P[0] - hw, P[1] - hh, P[2]],
    [P[0] + hw, P[1] - hh, P[2]],
    [P[0] - hw, P[1] + hh, P[2]],
    [P[0] + hw, P[1] + hh, P[2]],
  ]
    .map((p) => projectNDC(basis, p, fov, aspect))
    .map((p) => (p ? [p[0] * c - p[1] * s, p[0] * s + p[1] * c] : p));
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
  // v9 keeps the rig close on purpose — the old 4.5-unit wide stages are what
  // made the picture feel far away. Variety now comes from bank and swing.
  assert.ok(spread <= 1.4, `the dolly drifts ${spread} units off the base radius`);
  const rolls = new Set(stops.map((s) => (s.roll ?? 0).toFixed(3)));
  const yaws = new Set(stops.map((s) => (s.yaw ?? 0).toFixed(3)));
  assert.ok(rolls.size >= 4, `only ${rolls.size} distinct rolls`);
  assert.ok(yaws.size >= 4, `only ${yaws.size} distinct yaw offsets`);
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

// ---------------------------------------------------------------------------
// per-word choreography (`--groups auto`)
// ---------------------------------------------------------------------------

test('word groups cover every glyph and never straddle a space', () => {
  const lines = birdLines();
  for (let i = 0; i < lines.length; i++) {
    const gs = wordGroups(lines[i].text, lines[i].charTimes, i);
    assert.ok(gs && gs.length, `line ${i} grouped to nothing`);
    const chars = Array.from(lines[i].text);
    let cursor = 0;
    for (const g of gs) {
      assert.equal(g.i0, cursor, `line ${i} group does not start where the last one ended`);
      assert.ok(g.i1 > g.i0, `line ${i} has an empty group`);
      cursor = g.i1;
      assert.notEqual(chars[g.i0], ' ', `line ${i} group starts on a space`);
      // A group may ride a trailing space (the word plus the gap after it),
      // but never a leading one.
      if (chars[g.i1 - 1] === ' ') {
        assert.ok(g.i1 === chars.length || chars[g.i1] !== ' ', `line ${i} swallows two spaces`);
      }
    }
    assert.equal(cursor, chars.length, `line ${i} does not cover the whole line`);
  }
});

test('a group lands on its own first glyph, never later', () => {
  const lines = birdLines();
  for (let i = 0; i < lines.length; i++) {
    const gs = wordGroups(lines[i].text, lines[i].charTimes, i);
    for (const g of gs) {
      assert.equal(g.t, lines[i].charTimes[g.i0], `line ${i} group does not carry its first stamp`);
      for (let k = g.i0; k < g.i1; k++) {
        assert.ok(
          lines[i].charTimes[k] >= g.t,
          `line ${i} glyph ${k} is stamped before the group it belongs to`,
        );
      }
    }
  }
});

test('no two lines share a camera phrase', () => {
  const counts = birdGroups().map((g) => g.length);
  const choreo = assignChoreo(counts);
  const sigs = choreo.map(signatureOf);
  assert.equal(new Set(sigs).size, sigs.length, `only ${new Set(sigs).size} distinct phrases`);
  assert.equal(sigs.length, 33);
});

test('every line is cut into at least one group, most into several', () => {
  const counts = birdGroups().map((g) => g.length);
  for (const c of counts) assert.ok(c >= 1, 'a line was cut to nothing');
  const multi = counts.filter((c) => c > 1).length;
  assert.ok(multi >= 28, `only ${multi} of ${counts.length} lines have a phrase`);
});

test('choreographed stops never overlap and stay in time order', () => {
  const { stops } = birdGroupedLayout();
  assert.ok(stops.length > 34, `only ${stops.length} stops, expected a move per word group`);
  for (let i = 1; i < stops.length; i++) {
    const prev = stops[i - 1];
    assert.ok(
      prev.at + prev.duration <= stops[i].at + 1e-6,
      `stop ${i} at ${stops[i].at} starts before stop ${i - 1} finishes at ${prev.at + prev.duration}`,
    );
  }
});

test('a line is still framed at every stop of its own phrase', () => {
  const lines = birdLines();
  const { positions, stops } = birdGroupedLayout();
  for (let i = 0; i < lines.length; i++) {
    const [hw, hh] = boxHalfExtents(lines[i]);
    for (const s of phraseStops(lines, stops)[i]) {
      const pts = corners(positions[i], hw, hh, {
        radius: s.radius,
        elevation: s.elevation,
        target: s.target,
        yaw: evalOrbit(CAMERA, s.at + s.duration).yaw + (s.yaw ?? 0),
        roll: s.roll ?? 0,
      });
      for (const p of pts) {
        assert.ok(p, `line ${i} has a corner behind the camera mid-phrase`);
        assert.ok(Math.abs(p[0]) <= 1.05, `line ${i} slides off the side: ${p[0].toFixed(3)}`);
        assert.ok(Math.abs(p[1]) <= 1.05, `line ${i} slides off the top: ${p[1].toFixed(3)}`);
      }
    }
  }
});

test('a phrase never runs into the flight that carries the camera away', () => {
  const lines = birdLines();
  const { stops } = birdGroupedLayout();
  for (let i = 0; i < lines.length; i++) {
    const own = phraseStops(lines, stops)[i];
    assert.ok(own.length >= 1, `line ${i} has no flight stop`);
    // The flight settles exactly on the cue, so every other stop of the phrase
    // begins after it and finishes before the next flight takes over.
    const flight = own[0];
    assert.ok(Math.abs(flight.at + flight.duration - lines[i].time) < 0.002);
    for (let k = 1; k < own.length; k++) {
      assert.ok(
        own[k].at >= flight.at + flight.duration - 1e-6,
        `stop ${k} of line ${i} begins inside the flight in`,
      );
    }
    if (i + 1 < lines.length) {
      const nextFlight = stops.find(
        (s) => Math.abs(s.at + s.duration - lines[i + 1].time) < 0.002,
      );
      assert.ok(own[own.length - 1].at + own[own.length - 1].duration <= nextFlight.at + 1e-6);
    }
  }
});

test('the opening move lands with the first word, not after it', () => {
  const lines = birdLines();
  const { stops } = birdGroupedLayout();
  const gs = wordGroups(lines[0].text, lines[0].charTimes, 0);
  assert.ok(gs.length >= 2);
  // Everything after the flight in and before the flight out is the opening
  // line's phrase; none of it may outlast the gap to the next cue.
  const own = phraseStops(lines, stops)[0];
  assert.ok(own.length >= 2, `the opening line gets ${own.length} stops`);
  const cue = lines[0].time;
  const nextCue = lines[1].time;
  for (const s of own.slice(1)) {
    assert.ok(s.duration <= 1.2, `opening move takes ${s.duration}s`);
    assert.ok(s.at >= cue - 1e-6, `opening move starts at ${s.at}, before its cue`);
    assert.ok(s.at + s.duration <= nextCue, `opening move runs past the next cue`);
  }
});

// The grouped layout is the one that gets rendered, so its screen invariants
// are checked against the compressed glyph times the render actually uses —
// grouping pulls every glyph of a word onto the group's first stamp.
test('grouping keeps the screen from sitting empty or doubling up', () => {
  const lines = birdLines();
  const { exits } = birdGroupedLayout();
  const cues = lines.map((l) => l.time);

  let worst = 0;
  for (let i = 0; i < lines.length - 1; i++) {
    if (exits[i] === null) continue;
    const blank = cues[i + 1] - exits[i];
    worst = Math.max(worst, blank);
    assert.ok(blank <= TRAVEL_MAX, `line ${i} leaves ${blank.toFixed(2)}s of empty screen`);
  }
  assert.ok(worst > 0.2, `outgoing line lingers ${worst.toFixed(2)}s into the next cue`);

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

  for (let i = 0; i < lines.length; i++) {
    assert.ok(exits[i] !== null, `line ${i} never leaves the shot`);
    const ct = groupCharTimes(lines[i].text, lines[i].charTimes, i);
    const reveal = Math.max(...ct);
    assert.ok(
      exits[i] >= reveal + 0.15 - 1e-6,
      `line ${i} fades at ${exits[i]}, its last word lands ${reveal}`,
    );
  }
});
