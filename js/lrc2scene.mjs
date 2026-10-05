#!/usr/bin/env node
// LRC → chamina scene.
//
//   node js/lrc2scene.mjs scenes/sample.lrc -o scenes/lyrics.json
//
// Reads a timed lyrics file, borrows the stage from a template scene (camera,
// lights, bloom, DOF, resolution) and swaps in one text block per lyric line.
// The result is a plain scene JSON that `chamina-render` consumes unchanged.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseLrc, applyOffset, suggestDuration, buildLyricTexts, buildTitleBlock, visibleWidth } from './lrc.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');

/**
 * Named background looks. `top` is the zenith, `bottom` the floor, `accent`
 * colours the nebula band and horizon glow; `nebula`/`stars`/`drift` are the
 * 0..1 sliders. Declared before USAGE because the help text lists them.
 */
const LOOKS = {
  night: { top: '#01040f', bottom: '#05060f', accent: '#2f6dff', nebula: 0.55, stars: 0.75, drift: 1.0 },
  void: { top: '#000000', bottom: '#02030a', accent: '#1b2c6b', nebula: 0.30, stars: 0.95, drift: 0.6 },
  nebula: { top: '#0a0320', bottom: '#12062e', accent: '#a24bff', nebula: 0.85, stars: 0.65, drift: 1.3 },
  aurora: { top: '#001a26', bottom: '#001014', accent: '#00ffb3', nebula: 0.75, stars: 0.50, drift: 0.9 },
  ember: { top: '#1a0503', bottom: '#0a0201', accent: '#ff6a1e', nebula: 0.70, stars: 0.35, drift: 1.1 },
  ice: { top: '#041224', bottom: '#02060e', accent: '#8fe3ff', nebula: 0.45, stars: 0.80, drift: 0.8 },
  dawn: { top: '#2a0a3a', bottom: '#4a1420', accent: '#ff9a3c', nebula: 0.60, stars: 0.30, drift: 1.0 },
  bloom: { top: '#1a0226', bottom: '#26043a', accent: '#ff3ea5', nebula: 0.80, stars: 0.50, drift: 1.2 },
};

const USAGE = `chamina lrc → scene
  node js/lrc2scene.mjs <song.lrc> [options]

  -o, --out <file>       output scene JSON (default <lrc>.json next to input)
  -t, --template <file>  stage to reuse (default scenes/gift.json)
      --size <em>        lyric glyph size (default 1.4)
      --y <em>           vertical position of every line (default -0.4)
      --depth <em>       extrusion depth (default 0.09)
      --color <#rrggbb>  line colour (default #ffffff)
      --strength <n>     neon strength (default 1.6)
      --outline <#rrggbb>  rim colour; omit for no outline
      --outline-width <em> rim width (default 0.02)
      --bloom <n>        bloom intensity (default 0.06)
                         the gift-card preset uses 0.3, which floods the
                         gaps between strokes of a whole lyric line
      --entrance <mode>  none | slide | fall | pop (default slide)
                   a line may override it with a leading tag, e.g.
                   [00:16.575]{e:fall}远离那些繁华悲伤 …
      --jitter <amp>     idle wobble on every line, world units (default off)
      --jitter-rot <rad> rotational part of the wobble (default 0)
      --jitter-freq <hz> wobble rate (default 5)
      --wave <amp>       karaoke bounce travelling across each line (default off)
      --wave-freq <hz>   bounce rate (default 1.6)
      --wave-spacing <r> phase gap per character (default 0.55)
      --dolly <u>        ease the camera radius to this distance (default off)
      --crane <rad>      ease the orbit elevation to this angle (default off)
      --move-at <sec>    when the camera move starts (default 12% of scene)
      --move-dur <sec>   how long the camera move takes (default 76%)
      --shake <amp>      hand-held camera shake, world units (default off)
      --shake-roll <rad> roll wobble in radians (default 0.008)
      --shake-freq <hz>  shake rate (default 3.5)
      --fit-width <u>    world-space width budget per line; default 0.85 of
                         the frame width the camera can see, 0 disables
      --offset <sec>     shift every cue (added on top of the LRC offset tag)
      --fade-in <sec>    (default 0.3)
      --fade-out <sec>   (default 0.45)
      --stagger <sec>    per-character delay (default 0.04)
      --tail <sec>       breathing room after the last line (default 3)
      --duration <sec>   force the scene length instead of deriving it
      --yaw-drift <rad>  total camera sway across the scene; the orbit starts
                         and ends half of this away from head-on, so the
                         text is never read from behind (default 0.6)
                         0 locks the camera to a straight-on view
      --radius <u>       camera distance from the target (default from template)
      --keep-camera      leave the template's camera_animation untouched
      --keep-open        keep the last line up until the scene ends
      --outro-at <sec>   close the video with a second title card, fading in
                         here; the held last line hands the frame over at
                         outro-at - 0.6 instead of riding out the whole outro
      --outro-text <t>   outro card text (default --title)
      --outro-out <sec>  outro fade-out start (default duration - 1.5)
      --bg-look <name>   replace the flat colour with an animated sky that
                         drifts, glows and twinkles: ${Object.keys(LOOKS).join(' | ')}
      --bg-cut <t>:<n>   cut to look <n> at second <t>; repeat for more cuts
                   <n> may instead be an inline palette, so one song can cut
                   dozens of times without adding a named look:
                     <t>:#top/#bottom/#accent[@nebula,stars,drift]
                     --bg-cut 24.152:#00303a/#01121a/#6cffd0@0.75,0.7,1.1
      --bg-fade <sec>    crossfade length for every cut (default 1.5, 0 = hard)
      --fx               grade/distortion rack: vignette + chromatic + cue flash
                         split + saturation, plus a white flash and a small
                         camera push on every lyric cue
      --fx-flash <n>     cue flash, 0..1 (default 0.22)
      --fx-punch <n>     cue camera push in world units (default 0.35)
      --fx-vignette <n>  corner darkening 0..1 (default 0.55)
      --fx-chromatic <n> RGB split at the frame edge (default 0.0016)
      --fx-sat <n>       extra saturation, 1 = off (default 1.12)
      --title <text>     opening title card shown before the first lyric
      --subtitle <text>  second line of the title card
      --title-in <sec>   title fade-in start (default 1.2)
      --title-out <sec>  title fade-out start (default first cue - 1.2)
      --title-size <em>  title size (default 3), subtitle is a third of it
  -h, --help`;

function parseArgs(argv) {
  const o = {
    input: null,
    out: null,
    template: path.join(ROOT, 'scenes', 'gift.json'),
    size: 1.4,
    y: -0.4,
    depth: 0.09,
    color: '#ffffff',
    strength: 1.6,
    outline: null,
    outlineWidth: 0.02,
    bloom: 0.06,
    entrance: 'slide',
    jitter: null,
    jitterRot: 0,
    jitterFreq: 5,
    wave: null,
    waveFreq: 1.6,
    waveSpacing: 0.55,
    dolly: null,
    crane: null,
    moveAt: null,
    moveDur: null,
    shake: null,
    shakeRoll: 0.008,
    shakeFreq: 3.5,
    fitWidth: null,
    offset: 0,
    fadeIn: 0.3,
    fadeOut: 0.45,
    stagger: 0.04,
    tail: 3,
    duration: null,
    yawDrift: 0.6,
    radius: null,
    keepCamera: false,
    keepOpen: false,
    outroAt: null,
    outroText: null,
    outroOut: null,
    bgLook: null,
    bgCut: [],
    bgFade: 1.5,
    fx: false,
    fxFlash: null,
    fxPunch: null,
    fxVignette: null,
    fxChromatic: null,
    fxSat: null,
    title: null,
    subtitle: null,
    titleIn: 1.2,
    titleOut: null,
    titleSize: 3,
    help: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => {
      const v = argv[++i];
      if (v === undefined) throw new Error(`${a} needs a value`);
      return v;
    };
    switch (a) {
      case '-o': case '--out': o.out = next(); break;
      case '-t': case '--template': o.template = next(); break;
      case '--size': o.size = Number(next()); break;
      case '--y': o.y = Number(next()); break;
      case '--depth': o.depth = Number(next()); break;
      case '--color': o.color = next(); break;
      case '--strength': o.strength = Number(next()); break;
      case '--outline': o.outline = next(); break;
      case '--outline-width': o.outlineWidth = Number(next()); break;
      case '--bloom': o.bloom = Number(next()); break;
      case '--entrance': o.entrance = next(); break;
      case '--jitter': o.jitter = Number(next()); break;
      case '--jitter-rot': o.jitterRot = Number(next()); break;
      case '--jitter-freq': o.jitterFreq = Number(next()); break;
      case '--wave': o.wave = Number(next()); break;
      case '--wave-freq': o.waveFreq = Number(next()); break;
      case '--wave-spacing': o.waveSpacing = Number(next()); break;
      case '--dolly': o.dolly = Number(next()); break;
      case '--crane': o.crane = Number(next()); break;
      case '--move-at': o.moveAt = Number(next()); break;
      case '--move-dur': o.moveDur = Number(next()); break;
      case '--shake': o.shake = Number(next()); break;
      case '--shake-roll': o.shakeRoll = Number(next()); break;
      case '--shake-freq': o.shakeFreq = Number(next()); break;
      case '--fit-width': o.fitWidth = Number(next()); break;
      case '--offset': o.offset = Number(next()); break;
      case '--fade-in': o.fadeIn = Number(next()); break;
      case '--fade-out': o.fadeOut = Number(next()); break;
      case '--stagger': o.stagger = Number(next()); break;
      case '--tail': o.tail = Number(next()); break;
      case '--duration': o.duration = Number(next()); break;
      case '--yaw-drift': o.yawDrift = Number(next()); break;
      case '--radius': o.radius = Number(next()); break;
      case '--keep-camera': o.keepCamera = true; break;
      case '--keep-open': o.keepOpen = true; break;
      case '--outro-at': o.outroAt = Number(next()); break;
      case '--outro-text': o.outroText = next(); break;
      case '--outro-out': o.outroOut = Number(next()); break;
      case '--bg-look': o.bgLook = next(); break;
      case '--bg-cut': o.bgCut.push(next()); break;
      case '--bg-fade': o.bgFade = Number(next()); break;
      case '--fx': o.fx = true; break;
      case '--fx-flash': o.fxFlash = Number(next()); break;
      case '--fx-punch': o.fxPunch = Number(next()); break;
      case '--fx-vignette': o.fxVignette = Number(next()); break;
      case '--fx-chromatic': o.fxChromatic = Number(next()); break;
      case '--fx-sat': o.fxSat = Number(next()); break;
      case '--title': o.title = next(); break;
      case '--subtitle': o.subtitle = next(); break;
      case '--title-in': o.titleIn = Number(next()); break;
      case '--title-out': o.titleOut = Number(next()); break;
      case '--title-size': o.titleSize = Number(next()); break;
      case '-h': case '--help': o.help = true; break;
      default:
        if (!a.startsWith('-') && !o.input) o.input = a;
        else throw new Error(`unknown argument ${a}`);
    }
  }
  return o;
}

function resolve(p) {
  return path.isAbsolute(p) ? p : path.resolve(process.cwd(), p);
}

function clamp(v, lo, hi, fallback) {
  const n = Number(v);
  return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : fallback;
}

/**
 * Turn `--bg-look` / `--bg-cut` into the background timeline the renderer
 * crossfades through. Returns null when neither flag was given, so regenerating
 * an existing scene stays byte-identical.
 *
 * A cut's payload is either a LOOKS name or an inline palette, so one song can
 * cut thirty times without thirty entries in LOOKS:
 *
 *   --bg-cut 24.152:aurora
 *   --bg-cut 24.152:#00303a/#01121a/#6cffd0@0.75,0.7,1.1
 *                                 top     bottom    accent   nebula,stars,drift
 */
function buildBackgrounds(args) {
  const cuts = [];
  if (args.bgLook) cuts.push({ at: 0, name: args.bgLook });
  for (const raw of args.bgCut) cuts.push(parseCut(raw));
  if (cuts.length === 0) return null;
  // A first cut with no opener still needs something to cut *from*.
  if (cuts[0].at > 0) cuts.unshift({ at: 0, name: 'night' });
  for (const c of cuts) {
    if (c.palette) continue;
    if (!LOOKS[c.name]) {
      throw new Error(`unknown look "${c.name}" (have: ${Object.keys(LOOKS).join(', ')})`);
    }
  }
  cuts.sort((a, b) => a.at - b.at);
  const fade = clamp(args.bgFade, 0, 60, 1.5);
  return cuts.map((c, i) => ({
    at: i === 0 ? 0 : c.at,
    fade: i === 0 ? 0 : fade,
    ...(c.palette || LOOKS[c.name]),
  }));
}

const HEX6 = /^#[0-9a-fA-F]{6}$/;
const RATIO = (v, lo, hi, fallback) => {
  const n = Number(v);
  return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : fallback;
};

/** `<seconds>:<look>` or `<seconds>:#top/#bottom/#accent[@n,s,d]` → one cut. */
function parseCut(raw) {
  // A trailing ` # …` note is allowed; hexes never have whitespace before `#`.
  const s = String(raw);
  const note = s.search(/\s#/);
  const line = note < 0 ? s : s.slice(0, note);
  const i = line.lastIndexOf(':');
  if (i < 0) throw new Error(`--bg-cut wants <seconds>:<look>, got ${raw}`);
  const at = Number(line.slice(0, i));
  if (!Number.isFinite(at)) throw new Error(`--bg-cut: bad time in ${raw}`);
  const spec = line.slice(i + 1);
  if (!spec.includes('/')) return { at, name: spec };

  const parts = spec.split('/');
  if (parts.length !== 3) throw new Error(`--bg-cut palette wants top/bottom/accent, got ${raw}`);
  // The accent carries the sliders: `#4a6cff@0.55,0.85,1.0`.
  const [accent, sliders] = parts[2].split('@');
  const hexes = [parts[0], parts[1], accent];
  for (const hex of hexes) {
    if (!HEX6.test(hex)) throw new Error(`--bg-cut: "${hex}" is not #rrggbb in ${raw}`);
  }
  const palette = { top: parts[0], bottom: parts[1], accent, nebula: 0.6, stars: 0.6, drift: 1 };
  if (sliders) {
    const [n, st, d] = sliders.split(',');
    palette.nebula = RATIO(n, 0, 1, palette.nebula);
    palette.stars = RATIO(st, 0, 1, palette.stars);
    palette.drift = RATIO(d, 0, 4, palette.drift);
  }
  return { at, palette };
}

/** The grade/distortion rack, or null so the scene renders untouched. */
function buildFx(args) {
  if (!args.fx) return null;
  return {
    enabled: true,
    chromatic: clamp(args.fxChromatic, 0, 0.05, 0.0016),
    vignette: clamp(args.fxVignette, 0, 1, 0.55),
    saturation: clamp(args.fxSat, 0, 4, 1.12),
    flash: clamp(args.fxFlash, 0, 1, 0.22),
    punch: clamp(args.fxPunch, 0, 5, 0.35),
    hit_duration: 0.35,
  };
}

/**
 * The gift-card preset orbits a full turn in about sixty seconds, which walks
 * the camera behind the text plane — lyrics then read mirrored and at a
 * glancing angle. Swap in a slow symmetric sway about head-on instead: start
 * `yawDrift / 2` radians short of straight-on and cover exactly `yawDrift`
 * over the whole scene, so the text is always read from in front.
 */
function tuneOrbit(template, duration, args) {
  const base = template.camera_animation;
  const anim =
    base && base.type === 'orbit'
      ? { ...base }
      : { type: 'orbit', radius: 14, elevation: 0.09, target: [0, 1, 0] };
  if (Number.isFinite(args.radius) && args.radius > 0) anim.radius = args.radius;

  const drift = Number.isFinite(args.yawDrift) ? args.yawDrift : 0.6;
  anim.start_yaw = Math.PI / 2 - drift / 2;
  anim.speed = drift / Math.max(duration, 1e-3);
  return anim;
}

/**
 * Layer an eased camera move and/or a hand-held shake onto the orbit. Both are
 * optional; with no flags the orbit is returned untouched, so regenerating an
 * existing scene produces byte-identical output.
 */
function applyCameraMotion(anim, duration, args) {
  if (!anim || anim.type !== 'orbit') return anim;
  let out = anim;

  const hasMove = Number.isFinite(args.dolly) || Number.isFinite(args.crane);
  if (hasMove) {
    const move = { ease: 'ease_in_out_cubic' };
    if (Number.isFinite(args.dolly) && args.dolly > 0) move.radius_end = args.dolly;
    if (Number.isFinite(args.crane)) move.elevation_end = args.crane;
    move.at = Number.isFinite(args.moveAt) ? args.moveAt : duration * 0.12;
    move.duration = Number.isFinite(args.moveDur) ? args.moveDur : duration * 0.76;
    out = { ...out, movement: move };
  }

  if (Number.isFinite(args.shake) && args.shake > 0) {
    out = {
      ...out,
      shake: {
        amplitude: args.shake,
        frequency: Number.isFinite(args.shakeFreq) ? args.shakeFreq : 3.5,
        roll: Number.isFinite(args.shakeRoll) ? args.shakeRoll : 0.008,
        at: 0,
        until: null,
        ramp: 0.8,
      },
    };
  }
  return out;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help || !args.input) {
    console.log(USAGE);
    process.exit(args.help ? 0 : 1);
  }

  const lrcPath = resolve(args.input);
  const parsed = parseLrc(fs.readFileSync(lrcPath, 'utf8'));
  // The LRC `[offset:]` tag and `--offset` both nudge the whole song; the tag
  // runs first so the CLI flag stays an explicit authoring correction.
  const lines = applyOffset(parsed).map((l) => {
    const out = { time: l.time + (args.offset || 0), text: l.text };
    // Per-line `{e:fall}` overrides — without this the tag would be parsed and
    // then silently thrown away here.
    if (l.tags) out.tags = l.tags;
    return out;
  });
  if (!lines.length) throw new Error(`no timestamped lines in ${lrcPath}`);

  const template = JSON.parse(fs.readFileSync(resolve(args.template), 'utf8'));
  const duration = args.duration ?? suggestDuration(lines, args.tail);
  const cameraAnimation = applyCameraMotion(
    args.keepCamera ? template.camera_animation : tuneOrbit(template, duration, args),
    duration,
    args,
  );
  // Keep a margin: the outline rim, bloom bleed and DOF softness all push the
  // line outward beyond its advance width.
  const fitWidth = args.fitWidth ?? visibleWidth({ ...template, camera_animation: cameraAnimation }) * 0.85;

  const texts = buildLyricTexts(lines, {
    size: args.size,
    y: args.y,
    depth: args.depth,
    color: args.color,
    strength: args.strength,
    fadeIn: args.fadeIn,
    fadeOut: args.fadeOut,
    stagger: args.stagger,
    entrance: args.entrance,
    total: duration,
    fitWidth: fitWidth,
    jitter: Number.isFinite(args.jitter) && args.jitter > 0
      ? { amplitude: args.jitter, rotation: args.jitterRot, frequency: args.jitterFreq }
      : null,
    wave: Number.isFinite(args.wave) && args.wave > 0
      ? { amplitude: args.wave, frequency: args.waveFreq, spacing: args.waveSpacing }
      : null,
  });

  if (args.outline) {
    for (const t of texts) {
      t.material.outline = {
        color: args.outline,
        width: args.outlineWidth,
        strength: 1,
      };
    }
  }
  if (args.keepOpen) {
    const last = texts[texts.length - 1];
    const fade = last.animations.find((a) => a.type === 'fade');
    delete fade.fade_out_at;
    delete fade.fade_out;
  }

  // Outro card. The last cue lands far before the track ends, so `keep-open`
  // would pin one line on screen for the whole instrumental tail; hand the
  // frame over to a closing card instead.
  const r3 = (x) => Math.round(x * 1000) / 1000;
  const outroAt =
    Number.isFinite(args.outroAt) && args.outroAt > 0 ? args.outroAt : null;
  const outroOut =
    Number.isFinite(args.outroOut) && args.outroOut > 0
      ? args.outroOut
      : Math.max((outroAt ?? 0) + 3, duration - 1.5);
  if (outroAt !== null) {
    const fade = texts[texts.length - 1].animations.find((a) => a.type === 'fade');
    const handOff = r3(Math.max(fade.start_at + 2, outroAt - 0.6));
    if (!Number.isFinite(fade.fade_out_at) || fade.fade_out_at > handOff) {
      fade.fade_out = args.fadeOut;
      fade.fade_out_at = handOff;
    }
  }

  // Opening card: the track can start well before the first timed line, and a
  // bare particle field for that long reads as a stall. Default the fade-out
  // to just before the first cue so the card is gone before any lyric lands.
  const firstCue = lines.length ? lines[0].time : duration;
  const titleOut =
    Number.isFinite(args.titleOut) && args.titleOut >= 0
      ? args.titleOut
      : Math.max(args.titleIn + 2, firstCue - 1.2);
  const titleSize = clamp(args.titleSize, 0.2, 12, 3);
  const cardCommon = {
    color: args.color,
    strength: args.strength,
    outline: args.outline || undefined,
    outlineWidth: args.outlineWidth,
    startAt: args.titleIn,
    outAt: titleOut,
    // The card follows the chosen entrance; `none` still reads better as a
    // slide, since a title that simply appears is abrupt.
    entrance: args.entrance === 'none' ? 'slide' : args.entrance,
    stagger: 0.05,
  };
  const cards = [
    buildTitleBlock({
      ...cardCommon,
      text: args.title,
      size: titleSize,
      y: 1.2,
      fitWidth: fitWidth * 0.72,
    }),
    buildTitleBlock({
      ...cardCommon,
      text: args.subtitle,
      size: titleSize / 3,
      y: -0.6,
      strength: args.strength * 0.7,
      fitWidth: fitWidth * 0.5,
      stagger: 0.03,
    }),
    outroAt === null
      ? null
      : buildTitleBlock({
          ...cardCommon,
          text: args.outroText ?? args.title,
          size: titleSize,
          y: 1.2,
          fitWidth: fitWidth * 0.72,
          startAt: outroAt,
          outAt: outroOut,
          fadeOut: 1.4,
        }),
    outroAt === null
      ? null
      : buildTitleBlock({
          ...cardCommon,
          text: args.subtitle,
          size: titleSize / 3,
          y: -0.6,
          strength: args.strength * 0.7,
          fitWidth: fitWidth * 0.5,
          stagger: 0.03,
          startAt: r3(outroAt + 0.35),
          outAt: outroOut,
          fadeOut: 1.4,
        }),
  ].filter(Boolean);

  const scene = {
    ...template,
    duration,
    work_dir: template.work_dir || 'target/chamina',
    camera_animation: cameraAnimation,
    // Both omitted entirely when the flags are absent — `undefined` keys drop
    // out of JSON.stringify, which is what keeps old output byte-identical.
    backgrounds: buildBackgrounds(args) ?? undefined,
    fx: buildFx(args) ?? undefined,
    // A full lyric line is far more emissive area than the gift card's three
    // glyphs, so the preset's bloom intensity washes the stroke gaps shut.
    bloom: template.bloom
      ? { ...template.bloom, intensity: args.bloom }
      : undefined,
    // DOF has to focus on the plane the text sits on; a `--radius` override
    // would otherwise leave the whole line permanently defocused.
    dof: template.dof
      ? {
          ...template.dof,
          focal_distance:
            args.radius > 0 ? args.radius : template.dof.focal_distance,
        }
      : undefined,
    texts: cards.length ? [...cards, ...texts] : texts,
  };

  const outPath = args.out
    ? resolve(args.out)
    : lrcPath.replace(/\.lrc$/i, '') + '.json';
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify(scene, null, 2) + '\n', 'utf8');

  console.log(`${path.relative(ROOT, outPath)}`);
  console.log(
    `  ${texts.length} lines  → ${duration}s  → ` +
      `${(duration * (scene.fps || 60)).toFixed(0)} frames  → ` +
      `${scene.width}x${scene.height}` +
      (cards.length ? `  → title card ${args.titleIn}s→${titleOut}s` : ''),
  );
  const meta = parsed.meta;
  if (meta.ti || meta.ar) {
    console.log(`  ${meta.ti || ''}${meta.ar ? ' — ' + meta.ar : ''}`);
  }
}

try {
  main();
} catch (err) {
  console.error(String(err && err.message ? err.message : err));
  process.exit(1);
}
