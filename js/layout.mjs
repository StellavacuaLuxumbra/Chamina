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
  { rAdd: 4.5, sx: -0.22, sy: 0.24, el: 0.05 }, // 1 wide, upper-left
  { rAdd: 2.0, sx: 0.11, sy: -0.34, el: -0.03 }, // 2 mid, lower-right
  { rAdd: 3.0, sx: 0.0, sy: 0.44, el: 0.07 }, // 3 high angle, top third
  { rAdd: 4.5, sx: 0.22, sy: -0.16, el: -0.02 }, // 4 widest, right
  { rAdd: 1.0, sx: -0.05, sy: -0.44, el: 0.04 }, // 5 low close, bottom third
  { rAdd: 3.5, sx: 0.16, sy: 0.18, el: -0.06 }, // 6 wide diagonal
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

/** Is the axis-aligned box around `P` (half extents `hw`/`hh`) on screen? */
function boxOnFrame(state, P, hw, hh, fov, aspect) {
  const basis = cameraBasis(state);
  let behind = false;
  let any = false;
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const c of [
    [P[0] - hw, P[1] - hh, P[2]],
    [P[0] + hw, P[1] - hh, P[2]],
    [P[0] - hw, P[1] + hh, P[2]],
    [P[0] + hw, P[1] + hh, P[2]],
  ]) {
    const p = projectNDC(basis, c, fov, aspect);
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
  const baseR = Number(camera.radius) || 14;
  const rLo = Math.min(
    baseR,
    Number.isFinite(camera.movement?.radius_end) ? camera.movement.radius_end : baseR,
  );
  const rHi = Math.max(rmax, rLo + 4);

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

  // ---- one stop per line, easing in and settling exactly on the cue --------
  const starts = [];
  const durs = [];
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
    starts[i] = start;
    durs[i] = Math.max(cues[i] - start, 0.4);
  }

  const stops = [];
  for (let i = 0; i < n; i++) {
    const stage = stages[i % stages.length];
    const base = evalOrbit(rig, cues[i]);
    const radius = clamp(base.radius + stage.rAdd, rLo, rHi);
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
    stops.push({
      at: r3(starts[i]),
      duration: r3(durs[i]),
      ease: 'ease_in_out_cubic',
      target: [r3(P[0] - off[0]), r3(P[1] - off[1]), r3(P[2] - off[2])],
      radius: r3(radius),
      elevation: r3(elevation),
    });
  }

  // ---- return to the title framing before the outro -----------------------
  const tail = Number.isFinite(outroAt) ? outroAt : duration;
  const lastCue = cues[n - 1];
  let backAt = tail - 12;
  if (backAt < lastCue + 0.6) backAt = lastCue + 0.6;
  const backDur = tail - backAt;
  if (backDur >= 0.4) {
    const base = evalOrbit(rig, Math.min(tail, duration));
    stops.push({
      at: r3(backAt),
      duration: r3(backDur),
      ease: 'ease_in_out_cubic',
      target: base.target.map(r3),
      radius: r3(base.radius),
      elevation: r3(base.elevation),
    });
  }

  // ---- first moment each line is no longer in the shot --------------------
  const cam = { ...rig, stops };
  const exits = [];
  for (let i = 0; i < n; i++) {
    const P = positions[i];
    const from = Math.max(cues[i] + fadeIn + 0.15, revealEnd[i] + 0.15);
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
