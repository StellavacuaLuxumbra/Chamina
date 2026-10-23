// World-space lyric layout: where every line sits, where the camera looks when
// it lights up, and the moment it finally leaves the frame.
//
// The default scene is a single centred slot. Two lines cannot share it, so
// each one has to fade out before the next appears — which spends a fifth of
// every cue on a crossfade and pins all 33 lines to the same spot on screen.
//
// `layoutField` lays the song out as a serpentine path through the world and
// gives every cue its own camera stop instead:
//
//   * the camera stops **on** the cue, so a line is lit the moment it lands;
//   * a line is only faded once the camera has actually carried it out of
//     frame, so nothing disappears while it is still readable;
//   * framing cycles through a set of presets — high / low / wide / close,
//     left and right of centre;
//   * the wide presets dolly out, which buys their horizontal offset for free:
//     a wide shot has more room at the edges, so no line ever has to shrink to
//     make space. Close-ups keep the full fitted size.
//
// Everything here is a pure function of the cue times, the orbit and the
// flags, so regenerating from the same command reproduces the layout exactly.

import { evalOrbit, cameraBasis, projectNDC } from './anim.mjs';
import { wordGroups } from './lrc.js';

// Advance of a space in the font Rust lays out with. Measured against
// `total_width_em` on all 33 exported lines of scenes/bird.lrc to 4 decimals.
const SPACE_EM = 0.2959;

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const r3 = (x) => Math.round(x * 1000) / 1000;

function isCjk(ch) {
  const u = ch.codePointAt(0);
  return (
    (u >= 0x2000 && u <= 0x303f) ||
    (u >= 0x3040 && u <= 0x30ff) ||
    (u >= 0x3400 && u <= 0x4dbf) ||
    (u >= 0x4e00 && u <= 0x9fff) ||
    (u >= 0xf900 && u <= 0xfaff) ||
    (u >= 0xff00 && u <= 0xffef)
  );
}

/**
 * Advance width of a line in em, mirroring `text::layout` in Rust.
 *
 * Verified against the exported `total_width_em` for every word-timed line of
 * the bird PV. Anything that is neither CJK nor a space is counted a whole
 * em: an overestimate only ever makes the frame budget more conservative.
 */
export function emWidth(text) {
  let em = 0;
  for (const ch of String(text)) {
    if (ch === ' ') em += SPACE_EM;
    else if (isCjk(ch)) em += 1;
    else em += 1;
  }
  return em;
}

/**
 * Framing presets, cycled by line index. `rAdd` dollies out (which is what
 * creates room for `sx`), `sx`/`sy` are where in the frame the line sits in
 * normalised device coordinates, `el` tilts the orbit.
 *
 * The horizontal/vertical spans are deliberately modest: a slot pushes the
 * camera's target *away* from the neighbouring line, so an oversized `sy` on
 * one stop would hold the previous line on screen for several more cues.
 * Values are clamped to what the frame actually has room for at the chosen
 * radius, so a preset can never push a line past the edge.
 */
export const STAGES = [
  { rAdd: 0.0, sx: 0.0, sy: 0.0, el: 0.0 }, // 0 centre close-up
  { rAdd: 0.6, sx: -0.16, sy: 0.14, el: 0.03 }, // 1 slight wide, upper-left
  { rAdd: 0.25, sx: 0.07, sy: -0.2, el: -0.02 }, // 2 near, lower-right
  { rAdd: 0.8, sx: 0.0, sy: 0.24, el: 0.04 }, // 3 high angle, top third
  { rAdd: 0.45, sx: 0.14, sy: -0.08, el: -0.01 }, // 4 near, right
  { rAdd: 0.15, sx: -0.04, sy: -0.22, el: 0.02 }, // 5 low close, bottom third
  { rAdd: 0.65, sx: 0.1, sy: 0.1, el: -0.03 }, // 6 near diagonal
];

/**
 * Serpentine grid. Row 0 starts at the middle column and runs to the end, so
 * line 0 lands on the world origin where the default camera already looks and
 * the opening card is never re-framed; rows 1..n then snake left and right.
 *
 * `xStep` has to clear the line's own half-width plus the frame's half-width,
 * `yStep` the same vertically: a step that falls short leaves the outgoing line
 * sitting in shot alongside its successor, and the overlap test in the suite
 * fails the moment three lines share a frame.
 */
export const GRID = { cols: 5, xStep: 30, yStep: 15 };

/**
 * Seconds the rig may spend flying from one slot to the next. It is the whole
 * reason the screen is ever empty: the outgoing line leaves frame roughly
 * halfway through the flight, and the incoming one is not lit until the rig
 * settles on the beat — so the blank stretches with the flight. Capped here at
 * two seconds, that reads as a whip between holds instead of a drift through
 * an empty shot.
 */
export const TRAVEL_MAX = 2.0;

/** World position of lyric line `i` on the serpentine path. */
export function gridPositions(n, grid = GRID) {
  const { cols, xStep, yStep } = grid;
  const half = (cols - 1) / 2;
  const out = [];
  let row = 0;
  let col = half;
  let dir = 1;
  for (let i = 0; i < n; i++) {
    out.push([(col - half) * xStep, row * yStep, 0]);
    if (dir > 0) {
      if (col < cols - 1) col += 1;
      else {
        row += 1;
        dir = -1;
      }
    } else if (col > 0) {
      col -= 1;
    } else {
      row += 1;
      dir = 1;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// per-word camera choreography
// ---------------------------------------------------------------------------

/**
 * The vocabulary of a move. v9 leans on the two axes that keep the line in
 * shot: `dRoll` banks the camera around the view axis, `dYaw` swings it around
 * the line — both rotate the picture without carrying it away. `dR` zooms and
 * `dE` tilts; the four small pans survive only for the written opening and a
 * little variety, at a fraction of their old magnitude.
 */
const MOVES = {
  rollL: { dRoll: -1 },
  rollR: { dRoll: 1 },
  yawL: { dYaw: -1 },
  yawR: { dYaw: 1 },
  push: { dR: -1 },
  pull: { dR: 1 },
  crane: { dE: 1 },
  tilt: { dE: -1 },
  down: { dx: 0, dy: -1 },
  up: { dx: 0, dy: 1 },
  right: { dx: 1, dy: 0 },
  left: { dx: -1, dy: 0 },
};
const MOVE_NAMES = Object.keys(MOVES);
const EASE_NAMES = ['ease_in_out_cubic', 'ease_out_quad', 'ease_in_quad', 'smooth'];

function rng(seed) {
  let s = (seed >>> 0) || 1;
  return () => {
    s = (s + 0x6d2b79f5) >>> 0;
    let t = s;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/**
 * The opening line, written by hand. The first three beats of the song are the
 * whole pitch, so they are not left to the table: 远 drops into frame while the
 * camera falls faster than the glyph does, 离 gets a short settle, and then the
 * camera slides right on ease-in-out while 那些繁华悲伤 lands as one block.
 * The rest of the line falls back to the roll like everybody else.
 */
const OPENING_PREFIX = {
  kick: { kind: 'down', mag: 0.34, ease: 'ease_out_quad', lead: -0.12, pace: 0.85 },
  between: [
    { kind: 'up', mag: 0.13, ease: 'smooth', lead: 0.1, pace: 0.6 },
    { kind: 'right', mag: 0.31, ease: 'ease_in_out_cubic', lead: 0.14, pace: 0.8 },
  ],
};

/**
 * The bank and swing carried by each flight. A transition should read as the
 * world turning underneath the line rather than the camera backing away from
 * it: small yaw offsets stay inside the frame budget, the alternating signs
 * keep consecutive flights from looking like the same gesture.
 */
const FLY_YAW = [0.05, -0.07, 0.03, -0.05, 0.06, -0.03, 0.04];
const FLY_ROLL = [0.03, -0.04, 0.05, -0.02, 0.04, -0.05, 0.02];

/** Roll one move: which axis, how far, which ease, and whether it leads. */
function rollMove(r, kind) {
  const k = kind || MOVE_NAMES[Math.floor(r() * MOVE_NAMES.length)];
  const spec = MOVES[k];
  const mag =
    spec.dRoll !== undefined
      ? 0.05 + r() * 0.11
      : spec.dYaw !== undefined
        ? 0.07 + r() * 0.13
        : spec.dR !== undefined
          ? 0.5 + r() * 1.0
          : spec.dE !== undefined
            ? 0.02 + r() * 0.05
            : 0.06 + r() * 0.09;
  return {
    kind: k,
    mag,
    ease: EASE_NAMES[Math.floor(r() * EASE_NAMES.length)],
    // > 0 the camera settles before the word lands, < 0 it trails it.
    lead: -0.16 + r() * 0.42,
    // Fraction of the gap to the next beat the move is allowed to spend.
    pace: 0.5 + r() * 0.35,
  };
}

/**
 * One line's phrase: an optional kick on the beat itself, then one move per
 * gap between word groups. `groups` is the number of groups the line cuts to —
 * a short line therefore gets a short phrase, which is also what keeps a tight
 * gap from spilling into the flight that follows it.
 */
function rollChoreo(groups, seed) {
  const r = rng(seed);
  const kick = r() < 0.72 ? rollMove(r) : null;
  const between = [];
  for (let j = 1; j < groups; j++) between.push(rollMove(r));
  return { kick, between };
}

export function signatureOf(ch) {
  return `${ch.kick ? ch.kick.kind : '-'}>${ch.between.map((m) => m.kind).join(',')}`;
}

/**
 * Give every line its own phrase. Signatures are unique by construction: a
 * collision re-rolls that line with a new salt until the phrase is one nobody
 * else uses. Repetition is the one thing a lyric video cannot afford.
 *
 * Line 0 keeps the written opening in front of its roll, so the first three
 * beats never drift and the tail after them still earns its own signature.
 */
export function assignChoreo(groupCounts) {
  const used = new Set();
  return groupCounts.map((k, i) => {
    const prefix = i === 0 ? OPENING_PREFIX : null;
    const need = Math.max(0, k - 1);
    const kept = prefix ? Math.min(prefix.between.length, need) : 0;
    for (let salt = 0; salt < 80; salt++) {
      const r = rng(i * 7919 + salt * 104729 + 17);
      const kick = prefix ? prefix.kick : r() < 0.72 ? rollMove(r) : null;
      const between = prefix ? prefix.between.slice(0, kept) : [];
      for (let j = kept; j < need; j++) between.push(rollMove(r));
      const ch = { kick, between };
      const sig = signatureOf(ch);
      if (!used.has(sig)) {
        used.add(sig);
        return ch;
      }
    }
    return rollChoreo(k, i + 1);
  });
}

/** Does the line still fit the frame from this framing? */
function framingFits(P, target, radius, elevation, yaw, roll, hw, hh, th, aspect, fov) {
  const basis = cameraBasis({ radius, elevation, target, yaw });
  // A rolled picture rotates the line's screen box; inflate the box by the
  // roll's axis-aligned extent and test the four corners, because once the eye
  // has swung off the line's axis the near edge projects further out than the
  // centre would suggest.
  const c = Math.abs(Math.cos(roll || 0));
  const s = Math.abs(Math.sin(roll || 0));
  const ew = hw * c + hh * s;
  const eh = hh * c + hw * s;
  for (const pt of [
    [P[0] - ew, P[1] - eh, P[2]],
    [P[0] + ew, P[1] - eh, P[2]],
    [P[0] - ew, P[1] + eh, P[2]],
    [P[0] + ew, P[1] + eh, P[2]],
  ]) {
    const p = projectNDC(basis, pt, fov, aspect);
    if (!p) return false;
    // 7% of the frame is kept back for the outline rim and the bloom bleed.
    if (Math.abs(p[0]) > 0.93 || Math.abs(p[1]) > 0.94) return false;
  }
  return true;
}

/** Is the axis-aligned box around `P` (half extents `hw`/`hh`) on screen? */
function boxOnFrame(state, P, hw, hh, fov, aspect) {
  const basis = cameraBasis(state);
  const c = Math.abs(Math.cos(state.roll || 0));
  const s = Math.abs(Math.sin(state.roll || 0));
  const ew = hw * c + hh * s;
  const eh = hh * c + hw * s;
  let behind = false;
  let any = false;
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const c2 of [
    [P[0] - ew, P[1] - eh, P[2]],
    [P[0] + ew, P[1] - eh, P[2]],
    [P[0] - ew, P[1] + eh, P[2]],
    [P[0] + ew, P[1] + eh, P[2]],
  ]) {
    const p = projectNDC(basis, c2, fov, aspect);
    if (p === null) {
      behind = true;
      continue;
    }
    any = true;
    if (p[0] < minX) minX = p[0];
    if (p[0] > maxX) maxX = p[0];
    if (p[1] < minY) minY = p[1];
    if (p[1] > maxY) maxY = p[1];
  }
  // Every corner behind the eye: the box is out of the shot entirely.
  if (!any) return false;
  // Straddling the eye: part of it is right at the lens, so count it as seen.
  if (behind) return true;
  return !(maxX < -1 || minX > 1 || maxY < -1 || minY > 1);
}

/**
 * Lay `lines` out as a camera path.
 *
 * @param {Array<{time:number,text:string,charTimes?:number[]}>} lines lyric
 *   lines only — blanks already filtered, in scene order
 * @param {object} opts `camera` (orbit, no stops), `duration`, `fov`,
 *   `aspect`, `size`, `fitWidth`, `fadeIn`, `outroAt`, `titleOut`
 * @returns {{positions: number[][], exits: (number|null)[], stops: object[]}}
 *   `exits[i]` is an absolute `fade_out_at`, or `null` to leave the block's
 *   own timing alone (used for a `--keep-open` last line).
 */
export function layoutField(lines, opts = {}) {
  const {
    camera,
    duration,
    fov = 0.8726646,
    aspect = 16 / 9,
    size = 1.5,
    fitWidth,
    fadeIn = 0.3,
    outroAt = null,
    titleOut = null,
    grid = GRID,
    stages = STAGES,
    step = 0.1,
    rmax = 16,
    groups = 'off',
  } = opts;

  const n = lines.length;
  if (!n) return { positions: [], exits: [], stops: [] };

  const cues = lines.map((l) => l.time);
  const positions = gridPositions(n, grid);

  // Work from the rig without stops: the stops we are about to emit are built
  // on top of the orbit + `movement`, and `cameraAnimation` never has any yet
  // when this runs.
  const rig = { ...camera };
  delete rig.stops;

  const th = Math.tan(fov / 2);
  const halfWAt = (r) => th * aspect * r;
  const halfHAt = (r) => th * r;

  // The tightest the frame ever gets. `fitWidth` is budgeted against this same
  // radius, so keeping every stop above it is what makes the budget valid.
  // v9 keeps the rig within ±1.3 of the orbit's own radius *at that cue*, so
  // a shot can zoom a little but never picks up the old wide-stage distance
  // that made the line feel far away. `framingFits` still vetoes any stop
  // that would push the line past the edge.
  const baseR = Number(camera.radius) || 14;
  const rmaxCap = Number.isFinite(rmax) ? Math.min(rmax, baseR + 1.3) : baseR + 1.3;

  // World half-extents of each line, matching how Rust resolves `fit_width`:
  // the line shrinks to the budget, and its em box shrinks with it.
  const boxW = [];
  const boxH = [];
  const revealEnd = [];
  for (let i = 0; i < n; i++) {
    const em = emWidth(lines[i].text);
    const width = em > 0 ? Math.min(em * size, fitWidth) : 0;
    const resolved = em > 0 ? width / em : size;
    boxW[i] = width / 2;
    // Half the em box plus a little for the outline rim and bloom bleed.
    boxH[i] = resolved * 0.55;
    const ct = lines[i].charTimes;
    revealEnd[i] = ct && ct.length ? Math.max(...ct) : cues[i];
  }

  // ---- return to the title framing before the outro -----------------------
  const tail = Number.isFinite(outroAt) ? outroAt : duration;
  const lastCue = cues[n - 1];
  let backAt = tail - 12;
  if (backAt < lastCue + 0.6) backAt = lastCue + 0.6;
  const backDur = tail - backAt;

  // ---- flights in, one per line -------------------------------------------
  // Computed first: a line's own phrase may never run into the flight that
  // carries the camera away to the next one.
  const flightAt = [];
  const flightDur = [];
  for (let i = 0; i < n; i++) {
    const next = i + 1 < n ? cues[i + 1] : duration;
    const gap = Math.max(next - cues[i], 0.2);
    const capped = Math.min(14, gap - 0.4);
    // How long the rig takes to fly to this slot. Long enough to read as a
    // move, short enough that the line before it keeps its place on screen
    // right up to the beat: the old line only leaves frame about halfway into
    // the flight, so an unhurried drift parks the camera between two slots
    // with nothing lit at all. A capped flight is also the punchier read.
    const d = Math.max(1.2, Math.min(gap * 0.45, capped, TRAVEL_MAX));
    let start = cues[i] - d;
    // The rig eases stop-to-stop, so one stop may never begin before the
    // previous cue — otherwise a long gap (a whole instrumental bar) would
    // start reaching backwards into the line before it. Also hold the opening
    // card still until it is gone.
    if (i === 0 && Number.isFinite(titleOut)) start = Math.max(start, titleOut);
    else if (i > 0) start = Math.max(start, cues[i - 1] + 0.8);
    start = Math.min(start, cues[i] - 0.4);
    flightAt[i] = start;
    flightDur[i] = Math.max(cues[i] - start, 0.4);
  }

  // ---- per-word choreography ----------------------------------------------
  // With `groups: 'auto'` every line is cut into words that land together, and
  // each gap between them gets its own camera move. Off, and a line is one
  // stop exactly as before.
  const groupLists =
    groups === 'auto'
      ? lines.map((l, i) =>
          wordGroups(l.text, l.charTimes, i) || [{ i0: 0, i1: 1, t: cues[i], kind: -1 }],
        )
      : null;
  const choreo = groupLists ? assignChoreo(groupLists.map((g) => g.length)) : null;
  if (groupLists) {
    // The last glyph of a line now lands with its group, so the exit scan has
    // to wait for the group rather than for the glyph it used to wait for.
    // `groupCharTimes` also sweeps glyphs inside a group 35 ms apart.
    for (let i = 0; i < n; i++) {
      const g = groupLists[i][groupLists[i].length - 1];
      revealEnd[i] = g.t + Math.max(0, g.i1 - g.i0 - 1) * 0.035;
    }
  }

  const stops = [];
  const lineEnds = [];
  for (let i = 0; i < n; i++) {
    const stage = stages[i % stages.length];
    const base = evalOrbit(rig, cues[i]);
    // The zoom budget rides with the orbit: ±1.3 around wherever the long
    // dolly has the camera at this cue.
    const lineBase = base.radius;
    const rLo = Math.max(11.6, lineBase - 1.3);
    const rHi = Math.max(rLo + 0.8, Math.min(rmaxCap, lineBase + 1.3));
    const radius = clamp(lineBase + stage.rAdd, rLo, rHi);
    const elevation = clamp(base.elevation + stage.el, 0, 0.42);
    const fw = halfWAt(radius);
    const fh = halfHAt(radius);
    // Room the frame actually has once the line's own half-width and a margin
    // for the rim/bloom are taken out.
    const roomX = Math.max(0, 1 - 0.15 - boxW[i] / fw);
    const roomY = Math.max(0, 1 - boxH[i] / fh - 0.1);
    const sx = clamp(stage.sx, -roomX, roomX);
    const sy = clamp(stage.sy, -roomY, roomY);

    // Screen basis at the cue. `target` sits at the origin so this is just the
    // orientation; a target-independent basis is exactly what the slot needs.
    const basis = cameraBasis({ radius, elevation, target: [0, 0, 0], yaw: base.yaw });
    const P = positions[i];
    const off = [
      sx * fw * basis.right[0] + sy * fh * basis.up[0],
      sx * fw * basis.right[1] + sy * fh * basis.up[1],
      sx * fw * basis.right[2] + sy * fh * basis.up[2],
    ];
    const slot = [P[0] - off[0], P[1] - off[1], P[2] - off[2]];
    let flyYaw = FLY_YAW[i % FLY_YAW.length];
    let flyRoll = FLY_ROLL[i % FLY_ROLL.length];
    // The slot was solved for the plain orbit; if the banked framing cannot
    // hold the line, fly it level instead of shrinking the shot.
    if (!framingFits(P, slot, radius, elevation, base.yaw + flyYaw, flyRoll, boxW[i], boxH[i], th, aspect, fov)) {
      flyYaw = 0;
      flyRoll = 0;
    }
    stops.push({
      at: r3(flightAt[i]),
      duration: r3(flightDur[i]),
      ease: 'ease_in_out_cubic',
      target: slot.map(r3),
      radius: r3(radius),
      elevation: r3(elevation),
      yaw: r3(flyYaw),
      roll: r3(flyRoll),
    });

    let lastEnd = flightAt[i] + flightDur[i];
    if (!choreo) {
      lineEnds[i] = lastEnd;
      continue;
    }

    // The phrase: an optional kick on the beat itself, then one move per gap
    // between word groups. A tight line simply gets fewer moves, because the
    // flight to its successor claims the rest of the gap.
    const groups = groupLists[i];
    const deadline = i + 1 < n ? flightAt[i + 1] : backAt;
    const plan = [];
    if (choreo[i].kick) plan.push({ m: choreo[i].kick, on: cues[i], kick: true });
    for (let j = 1; j < groups.length; j++) {
      plan.push({ m: choreo[i].between[j - 1], on: groups[j].t, kick: false });
    }

    let curR = radius;
    let curE = elevation;
    let curYaw = flyYaw;
    let curRoll = flyRoll;
    for (let k = 0; k < plan.length; k++) {
      const { m, on, kick } = plan[k];
      // A move spends a fraction of the gap to whatever comes next. `lead` > 0
      // puts the camera ahead of the word, < 0 lets it trail — the two of them
      // together are what stop the camera from landing on the beat every time.
      const nextOn = k + 1 < plan.length ? plan[k + 1].on : deadline + 0.35;
      const span = Math.max(nextOn - on, 0.18);
      let dur = Math.max(0.18, Math.min(span * m.pace, 1.1));
      let at = kick ? on : on - m.lead - dur;
      at = Math.max(at, lastEnd + 0.02);
      if (at + dur > deadline) dur = deadline - at;
      if (dur < 0.12) continue;

      const spec = MOVES[m.kind];
      const dxN = (spec.dx || 0) * m.mag;
      const dyN = (spec.dy || 0) * m.mag;
      // Shrink the move until the line is still comfortably inside the frame
      // at the new framing: a dolly in takes room away, a swing turns the
      // line's screen offset, and a bank widens its box. Six halvings are the
      // budget; a move that still fails is dropped rather than forced.
      let scale = 1;
      let T = slot;
      let nR = curR;
      let nE = curE;
      let nY = curYaw;
      let nRo = curRoll;
      let ok = false;
      for (let tries = 0; tries < 6; tries++) {
        nR = clamp(curR + (spec.dR || 0) * m.mag * scale, rLo, rHi);
        nE = clamp(curE + (spec.dE || 0) * m.mag * scale, 0, 0.42);
        nY = clamp(curYaw + (spec.dYaw || 0) * m.mag * scale, -0.26, 0.26);
        nRo = clamp(curRoll + (spec.dRoll || 0) * m.mag * scale, -0.24, 0.24);
        const fw2 = halfWAt(nR);
        const fh2 = halfHAt(nR);
        // With the eye swung around the line, the pan direction turns too.
        const b2 = cameraBasis({ radius: nR, elevation: nE, target: [0, 0, 0], yaw: base.yaw + nY });
        T = [
          slot[0] + dxN * scale * fw2 * b2.right[0] + dyN * scale * fh2 * b2.up[0],
          slot[1] + dxN * scale * fw2 * b2.right[1] + dyN * scale * fh2 * b2.up[1],
          slot[2] + dxN * scale * fw2 * b2.right[2] + dyN * scale * fh2 * b2.up[2],
        ];
        if (framingFits(P, T, nR, nE, base.yaw + nY, nRo, boxW[i], boxH[i], th, aspect, fov)) {
          ok = true;
          break;
        }
        scale *= 0.5;
      }
      if (!ok) continue;
      // A move that ended up nowhere is not worth a stop: the rig eases
      // stop-to-stop, and a degenerate stop only steals time from the real one.
      if (
        nR === curR &&
        nE === curE &&
        nY === curYaw &&
        nRo === curRoll &&
        Math.abs(T[0] - slot[0]) + Math.abs(T[1] - slot[1]) + Math.abs(T[2] - slot[2]) < 1e-3
      ) {
        continue;
      }
      stops.push({
        at: r3(at),
        duration: r3(dur),
        ease: m.ease,
        target: T.map(r3),
        radius: r3(nR),
        elevation: r3(nE),
        yaw: r3(nY),
        roll: r3(nRo),
      });
      curR = nR;
      curE = nE;
      curYaw = nY;
      curRoll = nRo;
      lastEnd = at + dur;
    }
    lineEnds[i] = lastEnd;
  }

  if (backDur >= 0.4) {
    const base = evalOrbit(rig, Math.min(tail, duration));
    stops.push({
      at: r3(backAt),
      duration: r3(backDur),
      ease: 'ease_in_out_cubic',
      target: base.target.map(r3),
      radius: r3(base.radius),
      elevation: r3(base.elevation),
      // The title card is never banked: the closing shot has to sit level.
      yaw: 0,
      roll: 0,
    });
  }

  // ---- first moment each line is no longer in the shot --------------------
  const cam = { ...rig, stops };
  const exits = [];
  for (let i = 0; i < n; i++) {
    const P = positions[i];
    const from = Math.max(cues[i] + fadeIn + 0.15, revealEnd[i] + 0.15, lineEnds[i] + 0.15);
    let exit = null;
    for (let t = from; t <= duration; t += step) {
      if (!boxOnFrame(evalOrbit(cam, t), P, boxW[i], boxH[i], fov, aspect)) {
        exit = r3(t);
        break;
      }
    }
    exits.push(exit);
  }

  return { positions, exits, stops };
}
