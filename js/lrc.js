// Lyric timing (LRC) → chamina scene text blocks.
//
// The editor and the CLI both drive this: parse an .lrc file, then turn each
// timed line into its own entry of `texts[]` whose `fade.start_at` / `fade_out_at`
// are the line's cue times. Nothing here touches the DOM, so it runs both in the
// browser and under plain `node`.

const TIME_RE = /^\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/;
const META_RE = /^\[([A-Za-z_][\w#-]*):(.*)\]\s*$/;
// Per-line overrides ride in braces at the head of the lyric text:
//   [00:16.575]{e:fall}远离那些繁华悲伤 …
// Braces are never valid LRC lyric content, so the two syntaxes cannot clash.
const TAG_RE = /^\{([A-Za-z_][\w-]*)\s*[:=]\s*([^{}]*)\}/;
const ENTRANCES = ['none', 'slide', 'fall', 'pop'];

/**
 * Strip leading `{key:value}` overrides off a lyric line.
 *
 * @returns {{tags: Object<string,string>, text: string}} `tags` is empty for
 * an ordinary line, which is why callers only attach it when it has content —
 * that keeps tag-less scenes byte-identical.
 */
function splitTags(text) {
  const tags = {};
  let rest = String(text);
  let m = TAG_RE.exec(rest);
  while (m) {
    tags[m[1].toLowerCase()] = m[2].trim();
    rest = rest.slice(m[0].length);
    m = TAG_RE.exec(rest);
  }
  return { tags, text: rest.trim() };
}

function toSeconds(m) {
  const min = Number(m[1]);
  const sec = Number(m[2]);
  // .x = tenths, .xx = hundredths, .xxx = milliseconds.
  const frac = m[3] ? Number(m[3]) / 10 ** m[3].length : 0;
  return min * 60 + sec + frac;
}

/**
 * Parse LRC text.
 *
 * @returns {{meta: Object<string,string>, offsetMs: number,
 *            lines: Array<{time: number, text: string, tags?: Object<string,string>}>}}
 * `lines` is sorted by time; a line carrying several `[mm:ss.xx]` prefixes
 * becomes several entries (the standard repeat-line idiom). Per-line overrides
 * such as `{e:fall}` land in `tags`, and only when a line actually carries one.
 */
export function parseLrc(src) {
  const meta = {};
  let offsetMs = 0;
  const lines = [];

  for (const rawLine of String(src).split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) continue;

    // Collect every leading timestamp before deciding what this row is.
    const times = [];
    let rest = line;
    let m = TIME_RE.exec(rest);
    while (m) {
      times.push(toSeconds(m));
      rest = rest.slice(m[0].length);
      m = TIME_RE.exec(rest);
    }
    if (times.length) {
      const { tags, text } = splitTags(rest.replace(/^[\s:]+/, '').trim());
      for (const t of times) {
        const entry = { time: t, text };
        if (Object.keys(tags).length) entry.tags = tags;
        lines.push(entry);
      }
      continue;
    }

    const metaMatch = META_RE.exec(line);
    if (metaMatch) {
      const key = metaMatch[1].toLowerCase();
      const value = metaMatch[2].trim();
      meta[key] = value;
      if (key === 'offset') {
        const n = Number(value);
        if (Number.isFinite(n)) offsetMs = n;
      }
    }
  }

  lines.sort((a, b) => a.time - b.time || a.text.localeCompare(b.text));
  return { meta, offsetMs, lines };
}

/**
 * Shift every cue by `-offsetMs` — a positive `[offset:+500]` tag is the LRC
 * convention for "display the lyrics 500 ms earlier".
 */
export function applyOffset(parsed, offsetMs = parsed.offsetMs) {
  const shift = (Number(offsetMs) || 0) / 1000;
  return parsed.lines.map((l) => {
    const out = { time: l.time - shift, text: l.text };
    if (l.tags) out.tags = l.tags;
    return out;
  });
}

/** Length of the video that comfortably contains every line. */
export function suggestDuration(lines, tail = 3) {
  const last = lines.length ? lines[lines.length - 1].time : 0;
  return Math.max(4, Math.ceil(last + tail));
}

const clampNum = (v, fallback, lo, hi) => {
  const n = Number(v);
  if (!Number.isFinite(n)) return fallback;
  return Math.min(hi, Math.max(lo, n));
};

/**
 * Turn timed lines into a `texts[]` array.
 *
 * Every line becomes one text block centred on its `position`, hidden until
 * `start_at` and gone by `outAt` (normally the next line's cue), so only one
 * line is lit at a time. `entrance` adds a motion layer on top of the fade —
 * both layers share the same `start_at`, so the line slides/drops in while it
 * fades up.
 *
 * @param {Array<{time:number,text:string,tags?:Object<string,string>}>} lines
 * @param {object} opts see the destructuring below
 */
export function buildLyricTexts(lines, opts = {}) {
  const size = clampNum(opts.size, 1.5, 0.2, 12);
  const y = Number.isFinite(Number(opts.y)) ? Number(opts.y) : -0.4;
  const depth = clampNum(opts.depth, 0.09, 0.01, 2);
  const color = opts.color || '#ffffff';
  const strength = clampNum(opts.strength, 1.6, 0, 64);
  const offset = Number.isFinite(Number(opts.offset)) ? Number(opts.offset) : 0;
  const fadeIn = clampNum(opts.fadeIn, 0.3, 0, 10);
  const fadeOut = clampNum(opts.fadeOut, 0.45, 0, 10);
  const stagger = clampNum(opts.stagger, 0.04, 0, 1);
  const total = Number.isFinite(Number(opts.total)) ? Number(opts.total) : null;
  // Scene-wide default; a line may override it with a leading `{e:fall}` tag.
  const baseEntrance = opts.entrance || 'none';
  checkEntrance(baseEntrance);
  // World-space width budget. Rust resolves it against real font metrics at
  // export time, so we only have to say how much of the frame a line may use.
  const fitWidth = Number(opts.fitWidth);
  const fit = Number.isFinite(fitWidth) && fitWidth > 0 ? fitWidth : null;

  const out = [];
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (!line.text) continue;

    const entrance = lineEntrance(line, baseEntrance);
    const t = round3(line.time + offset);
    const nextRaw = lines[i + 1] ? lines[i + 1].time + offset : total;
    const next = Number.isFinite(nextRaw) ? nextRaw : t + 3;
    // Never let a line outlive the next cue, and always leave room to fade.
    const outAt = round3(Math.max(t + 0.1, Math.min(next, t + (opts.hold ?? 1e9)) - fadeOut));

    const animations = [
      {
        type: 'fade',
        duration: fadeIn,
        stagger,
        start_at: t,
        fade_out_at: outAt,
        fade_out: fadeOut,
      },
    ];
    if (entrance === 'slide') {
      animations.unshift({
        type: 'slide',
        delta: [-6, -2.5, 0],
        duration: 0.7,
        stagger: stagger * 1.5,
        ease: 'ease_out_quad',
        start_at: t,
      });
    } else if (entrance === 'fall') {
      animations.unshift({
        type: 'fall',
        duration: 0.7,
        stagger: stagger * 1.5,
        height: 1.4,
        gravity: -3,
        drift: 0,
        wobble: 0,
        start_at: t,
      });
    } else if (entrance === 'pop') {
      animations.unshift({
        type: 'pop',
        duration: 0.5,
        stagger: stagger * 1.5,
        start_at: t,
        from: 0,
        ease: 'back_out',
      });
    }

    const jit = buildJitter(opts.jitter, i, t);
    if (jit) animations.push(jit);
    const wav = buildWave(opts.wave, i, t);
    if (wav) animations.push(wav);

    const block = {
      text: line.text,
      size,
      depth,
      position: [0, y, 0],
      material: { type: 'neon', color, strength },
      alpha: 1,
      animations,
    };
    if (fit !== null) block.fit_width = fit;
    out.push(block);
  }
  return out;
}

function round3(v) {
  return Math.round(v * 1000) / 1000;
}

function checkEntrance(mode) {
  if (!ENTRANCES.includes(mode)) {
    throw new Error(`unknown entrance "${mode}" (have: ${ENTRANCES.join(', ')})`);
  }
  return mode;
}

/**
 * The entrance for one line: its `{e:fall}` tag if present, otherwise the
 * scene-wide `--entrance`. A typo'd mode throws rather than silently rendering
 * the wrong motion, because a whole line of the PV would be off.
 */
function lineEntrance(line, fallback) {
  const raw = line.tags && line.tags.e;
  if (raw === undefined || raw === '') return fallback;
  return checkEntrance(String(raw).trim().toLowerCase());
}

const num = (v, d) => (Number.isFinite(Number(v)) ? Number(v) : d);

/**
 * `jitter: 0.03` or `jitter: {amplitude, frequency, rotation, delay, ramp}` →
 * one `jitter` animation for line `i`, phased so neighbouring lines never wobble
 * in lockstep. `delay` holds the wobble back until the entrance has settled.
 */
function buildJitter(jitter, i, t) {
  if (!jitter) return null;
  const j = typeof jitter === 'object' ? jitter : { amplitude: jitter };
  const amp = Array.isArray(j.amplitude)
    ? j.amplitude.map((v) => num(v, 0))
    : (() => {
        const a = num(j.amplitude, 0.03);
        return [a, a * 0.8, 0];
      })();
  if (!amp.some((v) => v)) return null;
  return {
    type: 'jitter',
    amplitude: amp.map(round3),
    frequency: round3(num(j.frequency, 5)),
    rotation: round3(num(j.rotation, 0)),
    phase: round3(i * 1.7),
    start_at: round3(t + num(j.delay, 0.4)),
    stagger: round3(num(j.stagger, 0)),
    ramp: round3(num(j.ramp, 0.5)),
  };
}

/**
 * `wave: 0.2` or `wave: {amplitude, frequency, spacing, ramp}` → the karaoke
 * bounce: each character trails the one before it by `spacing` radians.
 */
function buildWave(wave, i, t) {
  if (!wave) return null;
  const w = typeof wave === 'object' ? wave : { amplitude: wave };
  const amp = num(w.amplitude, 0.2);
  if (!amp) return null;
  return {
    type: 'wave',
    amplitude: round3(amp),
    frequency: round3(num(w.frequency, 1.6)),
    spacing: round3(num(w.spacing, 0.55)),
    start_at: round3(t + num(w.delay, 0)),
    ramp: round3(num(w.ramp, 0.4)),
  };
}

/**
 * A standalone title/subtitle card: one text block that fades in and out on
 * fixed times instead of following an LRC cue. Used for the opening card, so
 * the gap between the start of the track and the first lyric is not empty.
 *
 * @param {object} opts {text, size, y, depth, color, strength, startAt, outAt,
 *                       fadeIn, fadeOut, stagger, entrance, fitWidth, outline}
 * @returns {object|null} null when `text` is blank
 */
export function buildTitleBlock(opts = {}) {
  const text = String(opts.text ?? '').trim();
  if (!text) return null;

  const size = clampNum(opts.size, 3, 0.2, 12);
  const y = Number.isFinite(Number(opts.y)) ? Number(opts.y) : 0.6;
  const depth = clampNum(opts.depth, 0.12, 0.01, 2);
  const color = opts.color || '#ffffff';
  const strength = clampNum(opts.strength, 1.6, 0, 64);
  const startAt = round3(Math.max(0, Number(opts.startAt) || 0));
  const fadeIn = clampNum(opts.fadeIn, 0.9, 0, 10);
  const fadeOut = clampNum(opts.fadeOut, 0.9, 0, 10);
  const stagger = clampNum(opts.stagger, 0.03, 0, 1);
  const outAt = Number(opts.outAt);
  const entrance = checkEntrance(opts.entrance || 'none');

  const animations = [];
  if (entrance === 'slide') {
    animations.push({
      type: 'slide',
      delta: [0, -1.6, 0],
      duration: 1.2,
      stagger,
      ease: 'ease_out_quad',
      start_at: startAt,
    });
  } else if (entrance === 'fall') {
    animations.push({
      type: 'fall',
      duration: 1.2,
      stagger,
      height: 1.8,
      gravity: -3,
      drift: 0,
      wobble: 0,
      start_at: startAt,
    });
  } else if (entrance === 'pop') {
    animations.push({
      type: 'pop',
      duration: 0.7,
      stagger,
      start_at: startAt,
      from: 0,
      ease: 'back_out',
    });
  }
  const fade = { type: 'fade', duration: fadeIn, stagger, start_at: startAt };
  if (Number.isFinite(outAt) && outAt > startAt) {
    fade.fade_out_at = round3(outAt);
    fade.fade_out = fadeOut;
  }
  animations.push(fade);

  const block = {
    text,
    size,
    depth,
    position: [0, y, 0],
    material: { type: 'neon', color, strength },
    alpha: 1,
    animations,
  };
  const fit = Number(opts.fitWidth);
  if (Number.isFinite(fit) && fit > 0) block.fit_width = fit;
  if (opts.outline) {
    block.material.outline = {
      color: opts.outline,
      width: Number(opts.outlineWidth) || 0.02,
      strength: 1,
    };
  }
  return block;
}

/**
 * Full width of the text plane the camera can actually see, in world units.
 * Used as the budget for `fit_width` so a line can never run off frame.
 */
export function visibleWidth(scene) {
  const cam = scene.camera || {};
  const anim = scene.camera_animation || {};
  let radius = Number(anim.radius) || 14;
  // A dolly-in narrows the frame as the scene plays; budget against the
  // tightest moment or a line fitted at t=0 runs off the edge later on.
  const mv = anim.movement;
  if (mv && Number.isFinite(mv.radius_end)) radius = Math.min(radius, mv.radius_end);
  const fov = Number(cam.fov) || 0.8726646;
  const aspect = (scene.width || 1920) / (scene.height || 1080);
  return 2 * Math.tan(fov / 2) * radius * aspect;
}

/**
 * One-shot convenience: LRC source → `{duration, texts}` ready to drop into a
 * scene file.
 */
export function lyricsToScene(src, opts = {}) {
  const parsed = parseLrc(src);
  const lines = applyOffset(parsed);
  const duration = opts.duration ?? suggestDuration(lines, opts.tail ?? 3);
  return {
    meta: parsed.meta,
    offsetMs: parsed.offsetMs,
    duration,
    texts: buildLyricTexts(lines, { ...opts, total: duration }),
  };
}
