// Lyric timing (LRC) → chamina scene text blocks.
//
// The editor and the CLI both drive this: parse an .lrc file, then turn each
// timed line into its own entry of `texts[]` whose `fade.start_at` / `fade_out_at`
// are the line's cue times. Nothing here touches the DOM, so it runs both in the
// browser and under plain `node`.

const TIME_RE = /^\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/;
const META_RE = /^\[([A-Za-z_][\w#-]*):(.*)\]\s*$/;
const HEX6_RE = /^#[0-9a-fA-F]{6}$/;
// Per-line overrides ride in braces at the head of the lyric text:
//   [00:16.575]{e:fall}远离那些繁华悲伤 …
// Braces are never valid LRC lyric content, so the two syntaxes cannot clash.
const TAG_RE = /^\{([A-Za-z_][\w-]*)\s*[:=]\s*([^{}]*)\}/;
// `overshoot` is `slide` + `pop` run together with an easing that passes 1.0:
// the line flies in from the lower-left *and* punches in from zero size, both
// landing past their rest pose and settling back. That reads as impact.
const ENTRANCES = ['none', 'slide', 'fall', 'pop', 'overshoot'];

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
 * Split a lyric body that carries per-glyph stamps into its plain text and one
 * appearance time per character.
 *
 * Word-timed LRC writes `<mm:ss.xx>` in front of a glyph — or in front of a
 * run of glyphs, which is how a *word* gets one stamp:
 *
 *   `<00:16.54>远<00:16.99>离 … 伤 <00:20.26>我 …`
 *
 * Characters after a stamp inherit it until the next stamp, so a multi-glyph
 * run stays a word and the untagged space between two words lights up with the
 * word it follows. Text ahead of the first stamp falls back to the line cue.
 *
 * @returns {{text: string, charTimes: number[]|null}} `charTimes` is `null`
 * when the line carries no stamps at all — that is what keeps an ordinary LRC
 * byte-identical.
 */
function splitGlyphTimes(src, lineTime) {
  // Built per call: a shared `/g` regex would leak `lastIndex` between lines.
  const re = /<(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?>|[^<]+|./g;
  let text = '';
  const charTimes = [];
  let at = null;
  let saw = false;
  for (let m = re.exec(src); m; m = re.exec(src)) {
    if (m[1] !== undefined) {
      at = toSeconds(m);
      saw = true;
      continue;
    }
    const when = at === null ? lineTime : at;
    for (const ch of m[0]) {
      text += ch;
      charTimes.push(when);
    }
  }
  if (!saw) return { text: src, charTimes: null };
  return { text, charTimes };
}

/**
 * Parse LRC text.
 *
 * @returns {{meta: Object<string,string>, offsetMs: number,
 *            lines: Array<{time: number, text: string, tags?: Object<string,string>,
 *                          charTimes?: number[]}>}}
 * `lines` is sorted by time; a line carrying several `[mm:ss.xx]` prefixes
 * becomes several entries (the standard repeat-line idiom). Per-line overrides
 * such as `{e:fall}` land in `tags`, and only when a line actually carries one.
 * A word-timed line additionally carries `charTimes` — one absolute time per
 * character — and only then.
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
      const glyph = splitGlyphTimes(text, times[0]);
      for (const t of times) {
        const entry = { time: t, text: glyph.text };
        if (Object.keys(tags).length) entry.tags = tags;
        // A repeated stamp (`[00:10][00:50]词 …`) replays the same run, so
        // slide its glyph times to sit on this copy's own line cue.
        if (glyph.charTimes) {
          const shift = t - times[0];
          entry.charTimes = shift === 0 ? glyph.charTimes : glyph.charTimes.map((v) => v + shift);
        }
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
    // Per-glyph times ride along with the line, shifted by the same amount —
    // they are meaningless relative to a cue that just moved.
    if (l.charTimes) out.charTimes = l.charTimes.map((v) => v - shift);
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

// ---------------------------------------------------------------------------
// word grouping
// ---------------------------------------------------------------------------

const thirds = (n) => {
  const a = Math.round(n / 3);
  const b = Math.round((n - a) / 2);
  return [a, b, n - a - b];
};

const chunks = (n, k) => {
  const out = [];
  let left = n;
  for (let i = k; i > 0; i--) {
    const take = Math.max(1, Math.round(left / i));
    out.push(Math.min(take, left - (i - 1)));
    left -= out[out.length - 1];
  }
  return out.filter((x) => x > 0);
};

/**
 * How a phrase gets cut into groups that land together. Every entry takes the
 * phrase length and returns piece sizes that sum to it.
 *
 * The list is deliberately wide: the same staging twice in a row is the one
 * thing a lyric video cannot afford, and 33 lines of different lengths have to
 * feel like 33 different ideas rather than one idea thirty-three times.
 */
const PHRASE_CUTS = [
  (n) => [n],
  (n) => (n >= 2 ? [1, n - 1] : [n]),
  (n) => (n >= 3 ? [2, n - 2] : [n]),
  (n) => (n >= 4 ? [1, 1, n - 2] : [n]),
  (n) => (n >= 2 ? [n - 1, 1] : [n]),
  (n) => (n >= 2 ? [Math.ceil(n / 2), Math.floor(n / 2)] : [n]),
  (n) => (n >= 3 ? thirds(n) : [n]),
  (n) => (n >= 4 ? [1, n - 2, 1] : [n]),
  (n) => chunks(n, 2),
  (n) => chunks(n, 3),
  (n) => (n >= 5 ? [1, 2, n - 3] : [n]),
  (n) => (n >= 5 ? [n - 3, 2, 1] : [n]),
  (n) => (n >= 6 ? [2, 2, n - 4] : [n]),
  (n) => (n >= 6 ? [3, 1, n - 4] : [n]),
  (n) => (n >= 3 ? [1, Math.ceil((n - 1) / 2), n - 1 - Math.ceil((n - 1) / 2)] : [n]),
];

export const PHRASE_CUT_COUNT = PHRASE_CUTS.length;

/**
 * Hand-worded segmentation for the fixed lyric corpus. Each entry is the line
 * text mapped to its words, `|`-separated; a trailing space stays on the word
 * before it so the pieces tile the whole string with no holes.
 *
 * The LRC carries one stamp per glyph and no word boundaries, and the random
 * phrase cuts above can land six glyphs at once — which is exactly the stiff
 * "block pop" the per-word reveal is meant to remove. The corpus is fixed, so
 * the words are written down instead of guessed.
 */
const WORD_SPLIT_SPECS = new Map(
  Object.entries({
    '远离那些繁华悲伤 我们从此告别了过往': '远离|那些|繁华|悲伤 |我们|从此|告别了|过往',
    '飞鸟张开时光翅膀 经历了无数风雨阻挡': '飞鸟|张开|时光|翅膀 |经历了|无数|风雨|阻挡',
    '来到她身旁 谁的目光 停留在那个梦里的夜晚':
      '来到|她|身旁 |谁的|目光 |停留|在|那个|梦里|的|夜晚',
    '我们追随风的方向 遥远飞行地老天荒 别彷徨': '我们|追随|风的|方向 |遥远|飞行|地老天荒 |别|彷徨',
    '在初遇的那地方 我们曾眷恋不已 梦想': '在|初遇|的|那地方 |我们|曾|眷恋|不已 |梦想',
    '面对迷惘 也不要放弃了希望': '面对|迷惘 |也|不要|放弃了|希望',
    '就像是盛开的花 对阳光不变的那信仰': '就像是|盛开|的|花 |对|阳光|不变的|那|信仰',
    '天空里早已留下了 永恒的神话': '天空里|早已|留下了 |永恒的|神话',
    '在夏天的操场 静静地眺望 在远方': '在|夏天|的|操场 |静静地|眺望 |在|远方',
    '连绵不断 是回忆之间的海洋': '连绵|不断 |是|回忆|之间|的|海洋',
    '穿过云端 是谁的天堂 在召唤': '穿过|云端 |是|谁的|天堂 |在|召唤',
    '她在寻访 那沉睡千年的答案': '她|在|寻访 |那|沉睡|千年|的|答案',
    '在告别的那地方 我们都无法放下的伤感':
      '在|告别|的|那地方 |我们|都|无法|放下|的|伤感',
    '挥一挥手留下的是美丽愿望': '挥一挥手|留下的|是|美丽|愿望',
    '就像是逆风的帆 对海浪不屈的那坚强': '就像是|逆风|的|帆 |对|海浪|不屈|的|那|坚强',
    '秋天的夜晚 静静地回想 在远方': '秋天|的|夜晚 |静静地|回想 |在|远方',
    '挥去不散 我们怀念着的故乡': '挥去|不散 |我们|怀念着|的|故乡',
    '在我们身旁': '在|我们|身旁',
    '她的目光 飞翔在每个梦里的夜晚': '她的|目光 |飞翔|在|每个|梦里|的|夜晚',
    '我们追随风的方向 遥远飞行地久天长 不返航':
      '我们|追随|风的|方向 |遥远|飞行|地久天长 |不|返航',
  }).map(([text, spec]) => [text, spec.split('|').map((w) => Array.from(w).length)]),
);

/**
 * Cuts written by hand instead of picked from the rhythm. The key is
 * `lineIndex:phraseIndex`.
 *
 * Line 0 is the signature: the singer takes 远 / 离 / 那些繁华悲伤 as three
 * separate gestures, and the rhythm that falls out of the formula lumps the
 * whole first phrase onto one beat. The second phrase is overridden to match —
 * the formula would hand it the same 1-1-rest shape as the first, and a line
 * that repeats its own rhythm reads as one idea said twice.
 */
const PHRASE_CUT_OVERRIDES = new Map([
  ['0:0', PHRASE_CUTS[3]],
  ['0:1', PHRASE_CUTS[12]],
]);

/**
 * Cut a word-timed line into groups that land together.
 *
 * A group always breaks at a space (the space rides with the word before it),
 * and each phrase between spaces is then cut by a rhythm picked from the line
 * and phrase index. Nothing here reads the audio: the rhythm is a function of
 * the text and the index alone, so regenerating reproduces it exactly.
 *
 * @param {string} text
 * @param {number[]} charTimes one absolute time per glyph
 * @param {number} seed line index
 * @returns {{i0:number,i1:number,t:number,kind:number}[]|null} half-open glyph
 *   ranges with the time the whole group lands on (its first glyph's stamp)
 */
export function wordGroups(text, charTimes, seed = 0) {
  if (!Array.isArray(charTimes) || !charTimes.length) return null;
  const chars = Array.from(text);
  const n = Math.min(chars.length, charTimes.length);
  if (!n) return null;
  const out = [];
  const spec = WORD_SPLIT_SPECS.get(text);
  if (spec) {
    let i = 0;
    for (const len of spec) {
      if (i >= n) break;
      const end = Math.min(i + len, n);
      out.push({ i0: i, i1: end, t: charTimes[i], kind: seed % PHRASE_CUTS.length });
      i = end;
    }
    // A corpus line that grew past its table entry keeps the remainder on the
    // last group rather than dropping glyphs on the floor.
    if (i < n) out.push({ i0: i, i1: n, t: charTimes[i], kind: seed % PHRASE_CUTS.length });
    return out.length ? out : null;
  }
  let phraseStart = 0;
  let phraseIndex = 0;
  const flush = (phraseEnd) => {
    const len = phraseEnd - phraseStart;
    if (len <= 0) return;
    const kind = (seed * 5 + phraseIndex * 3) % PHRASE_CUTS.length;
    const cut = PHRASE_CUT_OVERRIDES.get(`${seed}:${phraseIndex}`) || PHRASE_CUTS[kind];
    let i = phraseStart;
    for (const piece of cut(len)) {
      if (piece <= 0) continue;
      out.push({ i0: i, i1: i + piece, t: charTimes[i], kind });
      i += piece;
    }
    phraseIndex += 1;
  };
  for (let i = 0; i < n; i++) {
    if (chars[i] === ' ') {
      // Cut *after* the space, so the gap rides with the word it follows and
      // the groups tile the whole line with no holes.
      flush(i + 1);
      phraseStart = i + 1;
    }
  }
  flush(n);
  return out.length ? out : null;
}

/**
 * Compress per-glyph stamps onto their group, then sweep the glyphs inside a
 * group 35 ms apart. The word still lands on its first stamp, but its glyphs
 * arrive as a soft left-to-right sweep instead of a single hard pop — that
 * half-beat of spread is what keeps a word reveal from reading as a stamp
 * press. The last glyph of the line only moves later, and `buildLyricTexts`
 * waits on the last stamp for the fade-out, so nothing is cut short.
 */
export function groupCharTimes(text, charTimes, seed = 0) {
  const groups = wordGroups(text, charTimes, seed);
  if (!groups) return charTimes;
  const out = charTimes.slice();
  for (const g of groups) {
    for (let i = g.i0; i < g.i1; i++) out[i] = g.t + (i - g.i0) * 0.035;
  }
  return out;
}

/**
 * Turn timed lines into a `texts[]` array.
 *
 * Every line becomes one text block centred on its `position`, hidden until
 * `start_at` and gone by `outAt` (normally the next line's cue), so only one
 * line is lit at a time. `entrance` adds a motion layer on top of the fade —
 * both layers share the same `start_at`, so the line slides/drops in while it
 * fades up.
 *
 * A line carrying per-glyph stamps additionally gets `char_times`, and every
 * staggered layer then reads its delay from there instead of from
 * `start_at + idx * stagger`: the line arrives one word at a time, on the
 * singer's rhythm rather than on a fixed grid.
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
  let lyricIndex = 0;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (!line.text) continue;

    const entrance = lineEntrance(line, baseEntrance);
    const t = round3(line.time + offset);
    // Word-timed lines light up glyph by glyph, so the fade-out has to wait for
    // the last one — otherwise the tail of a line would start disappearing the
    // moment it appears. Absent per-glyph times nothing changes here.
    const charTimesRaw = Array.isArray(line.charTimes)
      ? line.charTimes.slice(0, line.text.length).map((v) => round3(v + offset))
      : null;
    // `groups: 'auto'` lands each word as one beat rather than glyph by glyph:
    // the stamps get compressed onto their group, which is what lets a camera
    // move ride the gap between words instead of being drowned by sixteen of
    // them. Off by default, so nothing about the plain word-timed export moves.
    const charTimes =
      charTimesRaw && opts.groups === 'auto'
        ? groupCharTimes(line.text, charTimesRaw, lyricIndex).map(round3)
        : charTimesRaw;
    const lastGlyph = charTimes && charTimes.length ? Math.max(...charTimes) : t;
    const nextRaw = lines[i + 1] ? lines[i + 1].time + offset : total;
    const next = Number.isFinite(nextRaw) ? nextRaw : t + 3;
    // Never let a line outlive the next cue, and always leave room to fade.
    let outAt = round3(Math.max(t + 0.1, Math.min(next, t + (opts.hold ?? 1e9)) - fadeOut));
    if (charTimes) outAt = round3(Math.max(outAt, lastGlyph + 0.15));

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
    } else if (entrance === 'overshoot') {
      // Two layers on purpose: `slide` contributes position, `pop` scale, and
      // evalAll adds offsets while multiplying scale, so they compose into one
      // entrance. `back_out` overshoots, so each lands past its rest pose.
      animations.unshift(
        {
          type: 'slide',
          delta: [-7, -3, 0],
          duration: 0.75,
          stagger: stagger * 1.5,
          ease: 'back_out',
          start_at: t,
        },
        {
          type: 'pop',
          duration: 0.6,
          stagger: stagger * 1.5,
          start_at: t,
          from: 0,
          ease: 'back_out',
        },
      );
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
    // One absolute start time per glyph. It rides on the block rather than on
    // any one animation because the fade, the entrance and the wobble all have
    // to fall on the same rhythm.
    if (charTimes) block.char_times = charTimes;
    out.push(block);
    lyricIndex += 1;
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
  const out = {
    type: 'jitter',
    amplitude: amp.map(round3),
    frequency: round3(num(j.frequency, 5)),
    rotation: round3(num(j.rotation, 0)),
    phase: round3(i * 1.7),
    start_at: round3(t + num(j.delay, 0.4)),
    stagger: round3(num(j.stagger, 0)),
    ramp: round3(num(j.ramp, 0.5)),
  };
  // Written only when asked for, so scenes without the flag stay
  // byte-identical.
  if (j.hit) {
    out.hit = true;
    out.hit_duration = round3(num(j.hit_duration, 0.55));
  }
  return out;
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
  const out = {
    type: 'wave',
    amplitude: round3(amp),
    frequency: round3(num(w.frequency, 1.6)),
    spacing: round3(num(w.spacing, 0.55)),
    start_at: round3(t + num(w.delay, 0)),
    ramp: round3(num(w.ramp, 0.4)),
  };
  if (w.hit) {
    out.hit = true;
    out.hit_duration = round3(num(w.hit_duration, 2.0));
  }
  return out;
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

// ---------------------------------------------------------------------------
// per-line colour palette
// ---------------------------------------------------------------------------

/**
 * `#rrggbb` → `{h, s, l}` in 0..1. Plain enough for tinting rims; not a colour
 * science conversion.
 */
export function hexHsl(hex) {
  const h = String(hex || '#ffffff').replace(/^#/, '');
  const n = parseInt(h.slice(0, 6), 16);
  const r = ((n >> 16) & 255) / 255;
  const g = ((n >> 8) & 255) / 255;
  const b = (n & 255) / 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const l = (max + min) / 2;
  if (max === min) return { h: 0, s: 0, l };
  const d = max - min;
  const s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
  let hh;
  if (max === r) hh = ((g - b) / d + (g < b ? 6 : 0)) / 6;
  else if (max === g) hh = ((b - r) / d + 2) / 6;
  else hh = ((r - g) / d + 4) / 6;
  return { h: hh, s, l };
}

const hcl = (x) => Math.round(Math.min(255, Math.max(0, x)) * 255);

function hue2rgb(p, q, t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1 / 6) return p + (q - p) * 6 * t;
  if (t < 1 / 2) return q;
  if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
  return p;
}

/** `{h, s, l}` in 0..1 → `#rrggbb`. */
export function hslHex(h, s, l) {
  const H = ((Number(h) || 0) % 1 + 1) % 1;
  const S = Math.min(1, Math.max(0, Number(s) || 0));
  const L = Math.min(1, Math.max(0, Number(l) || 0));
  if (S === 0) {
    const v = hcl(L);
    return `#${((v << 16) | (v << 8) | v).toString(16).padStart(6, '0')}`;
  }
  const q = L < 0.5 ? L * (1 + S) : L + S - L * S;
  const p = 2 * L - q;
  const r = hcl(hue2rgb(p, q, H + 1 / 3));
  const g = hcl(hue2rgb(p, q, H));
  const b = hcl(hue2rgb(p, q, H - 1 / 3));
  return `#${((r << 16) | (g << 8) | b).toString(16).padStart(6, '0')}`;
}

/**
 * Cycle a romantic palette across the lyric blocks. The rim follows each
 * line's tint, hue-shifted a little and pushed darker and more saturated, so
 * the outline stays a contour instead of competing with the glyph colour.
 * Mutates and returns `texts` (title/outro cards are not in this list).
 */
export function applyPalette(texts, hexes) {
  const pal = String(hexes)
    .split(',')
    .map((s) => s.trim())
    .filter((s) => HEX6_RE.test(s));
  if (!pal.length) throw new Error(`--palette wants comma-separated #rrggbb values`);
  texts.forEach((tx, i) => {
    const c = pal[i % pal.length];
    tx.material.color = c.toLowerCase();
    if (tx.material.outline) {
      const { h, s, l } = hexHsl(c);
      tx.material.outline.color = hslHex(
        h + 0.055,
        Math.min(1, s * 1.6 + 0.18),
        l * 0.6,
      );
    }
  });
  return texts;
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
