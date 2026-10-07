import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

import {
  parseLrc,
  applyOffset,
  suggestDuration,
  buildLyricTexts,
  buildTitleBlock,
} from './lrc.js';

const SRC = `[ti:鸟之诗]
[ar:HANA]
[offset:500]
[00:01.500]第一句
[00:03.00][00:10.00]重复的那句
[00:06.00]第二句
`;

test('parseLrc reads meta, offset and sorts lines', () => {
  const { meta, offsetMs, lines } = parseLrc(SRC);
  assert.equal(meta.ti, '鸟之诗');
  assert.equal(meta.ar, 'HANA');
  assert.equal(offsetMs, 500);
  assert.deepEqual(
    lines.map((l) => l.time),
    [1.5, 3, 6, 10],
  );
  assert.equal(lines[1].text, '重复的那句');
  assert.equal(lines[3].text, '重复的那句');
});

test('a positive [offset:] tag makes every cue earlier', () => {
  const lines = applyOffset(parseLrc(SRC));
  assert.equal(lines[0].time, 1);
  assert.equal(lines[1].time, 2.5);
});

test('the LRC offset does not leak into the line text', () => {
  assert.equal(applyOffset(parseLrc(SRC))[0].text, '第一句');
});

test('suggestDuration covers the last line plus the tail', () => {
  // last cue 10.00 minus the 500 ms LRC offset = 9.5, plus 3 s of tail
  assert.equal(suggestDuration(applyOffset(parseLrc(SRC)), 3), 13);
  assert.equal(suggestDuration([], 3), 4);
});

test('every lyric line hides again before the next cue lands', () => {
  const lines = applyOffset(parseLrc(SRC));
  const texts = buildLyricTexts(lines, { fadeIn: 0.3, fadeOut: 0.45, total: 40 });
  assert.equal(texts.length, 4);
  for (let i = 0; i < texts.length - 1; i++) {
    const fade = texts[i].animations.find((a) => a.type === 'fade');
    const next = texts[i + 1].animations.find((a) => a.type === 'fade');
    assert.ok(fade.fade_out_at <= next.start_at, `line ${i} outlives its neighbour`);
    assert.ok(fade.fade_out_at >= fade.start_at, `line ${i} fades out before it fades in`);
  }
});

test('the entrance layer and the fade share one start_at', () => {
  const texts = buildLyricTexts(applyOffset(parseLrc(SRC)), { entrance: 'slide' });
  for (const t of texts) {
    const times = t.animations.map((a) => a.start_at);
    assert.equal(new Set(times).size, 1, `mixed cues on "${t.text}"`);
    assert.ok(t.animations.some((a) => a.type === 'slide'));
    assert.ok(t.animations.some((a) => a.type === 'fade'));
  }
});

test('a blank line is skipped rather than emitted empty', () => {
  const texts = buildLyricTexts([{ time: 1, text: '' }, { time: 2, text: 'X' }]);
  assert.equal(texts.length, 1);
});

test('fit_width is only written when it is a positive budget', () => {
  assert.ok('fit_width' in buildLyricTexts([{ time: 1, text: 'X' }], { fitWidth: 12 })[0]);
  assert.ok(!('fit_width' in buildLyricTexts([{ time: 1, text: 'X' }], { fitWidth: 0 })[0]));
  assert.ok(!('fit_width' in buildLyricTexts([{ time: 1, text: 'X' }], {})[0]));
});

test('buildTitleBlock returns null for an absent card', () => {
  assert.equal(buildTitleBlock({}), null);
  assert.equal(buildTitleBlock({ text: '   ' }), null);
});

test('the title card fades in and out on the times it was given', () => {
  const card = buildTitleBlock({
    text: '鸟之诗',
    size: 3,
    startAt: 1.2,
    outAt: 15.4,
    fadeIn: 0.9,
    fadeOut: 0.9,
    entrance: 'slide',
    fitWidth: 14,
  });
  const fade = card.animations.find((a) => a.type === 'fade');
  const slide = card.animations.find((a) => a.type === 'slide');
  assert.equal(fade.start_at, 1.2);
  assert.equal(fade.duration, 0.9);
  assert.equal(fade.fade_out_at, 15.4);
  assert.equal(fade.fade_out, 0.9);
  assert.equal(slide.start_at, 1.2);
  assert.equal(card.fit_width, 14);
  assert.equal(card.size, 3);
});

test('without an out time the title card stays lit', () => {
  const card = buildTitleBlock({ text: '鸟之诗', startAt: 1.2 });
  const fade = card.animations.find((a) => a.type === 'fade');
  assert.ok(!('fade_out_at' in fade));
  assert.ok(!('fade_out' in fade));
});

test('a non-positive start time is clamped to zero', () => {
  assert.equal(buildTitleBlock({ text: 'x', startAt: -5 }).animations[0].start_at, 0);
});

const TAGGED = `[offset:500]
[00:01.000]{e:fall}第一句
[00:03.000]普通句
[00:05.000]{E=pop}{other:hi}  有空格的
`;

test('a leading {e:…} tag becomes a per-line override, not lyric text', () => {
  const { lines } = parseLrc(TAGGED);
  assert.equal(lines[0].text, '第一句');
  assert.deepEqual(lines[0].tags, { e: 'fall' });
  assert.equal(lines[1].text, '普通句');
  assert.ok(!('tags' in lines[1]), 'an ordinary line must not grow a tags key');
});

test('several braces are read left to right and the text is trimmed', () => {
  const { lines } = parseLrc(TAGGED);
  assert.deepEqual(lines[2].tags, { e: 'pop', other: 'hi' });
  assert.equal(lines[2].text, '有空格的');
});

test('tags survive the LRC offset shift', () => {
  const lines = applyOffset(parseLrc(TAGGED));
  assert.equal(lines[0].time, 0.5);
  assert.deepEqual(lines[0].tags, { e: 'fall' });
  assert.ok(!('tags' in lines[1]));
});

test('the entrance tag overrides the scene-wide --entrance', () => {
  const texts = buildLyricTexts(applyOffset(parseLrc(TAGGED)), { entrance: 'slide' });
  const kind = (t) => t.animations.map((a) => a.type);
  assert.ok(kind(texts[0]).includes('fall'), 'tagged line must fall');
  assert.ok(kind(texts[1]).includes('slide'), 'untagged line keeps the global mode');
  assert.ok(kind(texts[2]).includes('pop'), 'tag keys are case-insensitive');
});

test('every line still lights its entrance and its fade on one cue', () => {
  const texts = buildLyricTexts(applyOffset(parseLrc(TAGGED)), { entrance: 'slide' });
  for (const t of texts) {
    assert.equal(new Set(t.animations.map((a) => a.start_at)).size, 1, `mixed cues on "${t.text}"`);
  }
});

test('an unknown entrance is an error, not a silent fallback', () => {
  assert.throws(() => buildLyricTexts([{ time: 1, text: 'X' }], { entrance: 'wiggle' }), /unknown entrance/);
  assert.throws(
    () => buildLyricTexts([{ time: 1, text: 'X', tags: { e: 'wiggle' } }], { entrance: 'slide' }),
    /unknown entrance/,
  );
  assert.throws(() => buildTitleBlock({ text: 'X', entrance: 'wiggle' }), /unknown entrance/);
});

test('a tag never appears in the rendered text', () => {
  const texts = buildLyricTexts(applyOffset(parseLrc(TAGGED)), {});
  for (const t of texts) assert.ok(!t.text.includes('{'), `brace leaked into "${t.text}"`);
});

test('a motion gate is written only when it is asked for', () => {
  const lines = applyOffset(parseLrc(SRC));

  const off = buildLyricTexts(lines, { jitter: { amplitude: 0.035 }, wave: { amplitude: 0.09 } });
  for (const t of off) {
    const j = t.animations.find((a) => a.type === 'jitter');
    const w = t.animations.find((a) => a.type === 'wave');
    assert.equal(j.hit, undefined, 'no flag → no key, so the scene stays byte-stable');
    assert.equal(j.hit_duration, undefined);
    assert.equal(w.hit, undefined);
    assert.equal(w.hit_duration, undefined);
  }

  const on = buildLyricTexts(lines, {
    jitter: { amplitude: 0.035, hit: true, hit_duration: 0.8 },
    wave: { amplitude: 0.09, hit: true },
  });
  for (const t of on) {
    const j = t.animations.find((a) => a.type === 'jitter');
    const w = t.animations.find((a) => a.type === 'wave');
    assert.equal(j.hit, true);
    assert.equal(j.hit_duration, 0.8, 'the caller can tune the shudder length');
    assert.equal(w.hit, true);
    assert.equal(w.hit_duration, 2, 'the ripple default matches Rust');
  }
});

const WORDS = `[00:10.00]<00:10.00>远<00:10.40>离 那<00:11.00>些
[00:20.00]直白的一行
`;

test('per-glyph stamps become one appearance time per character', () => {
  const { lines } = parseLrc(WORDS);
  assert.equal(lines.length, 2);
  assert.equal(lines[0].text, '远离 那些');
  assert.equal(lines[0].charTimes.length, lines[0].text.length);
  assert.equal(lines[0].charTimes[0], 10);
  // A stamp covers everything up to the next one, so a word moves as a unit
  // and the untagged space rides with the word it follows.
  assert.equal(lines[0].charTimes[1], 10.4);
  assert.equal(lines[0].charTimes[3], 10.4);
  assert.equal(lines[0].charTimes[4], 11);
});

test('a line without stamps carries no charTimes at all', () => {
  for (const l of parseLrc(SRC).lines) assert.equal(l.charTimes, undefined);
  assert.equal(parseLrc(WORDS).lines[1].charTimes, undefined);
});

test('the LRC offset carries the glyph times too', () => {
  const lines = applyOffset(parseLrc('[offset:500]\n[00:10.00]<00:10.00>远<00:10.40>离\n'));
  assert.equal(lines[0].time, 9.5);
  assert.ok(Math.abs(lines[0].charTimes[0] - 9.5) < 1e-9);
  assert.ok(Math.abs(lines[0].charTimes[1] - 9.9) < 1e-9);
});

test('a repeated line stamps its glyphs on its own cue', () => {
  const { lines } = parseLrc('[00:10.00][00:50.00]<00:10.00>词<00:10.50>句\n');
  assert.deepEqual(lines[0].charTimes, [10, 10.5]);
  assert.deepEqual(lines[1].charTimes, [50, 50.5]);
});

test('word timing reaches the block as char_times, and only then', () => {
  const texts = buildLyricTexts(applyOffset(parseLrc(WORDS)), { entrance: 'pop' });
  assert.equal(texts[0].char_times.length, texts[0].text.length);
  const fade = texts[0].animations.find((a) => a.type === 'fade');
  const pop = texts[0].animations.find((a) => a.type === 'pop');
  // Both layers keep the line cue as their start_at — the per-glyph stamps
  // only decide when each character of it fires.
  assert.equal(fade.start_at, texts[0].char_times[0]);
  assert.equal(pop.start_at, fade.start_at);
  assert.equal(texts[1].char_times, undefined, 'an unstamped line stays untouched');
});

test('a line never starts fading before its last glyph appeared', () => {
  const texts = buildLyricTexts(applyOffset(parseLrc(WORDS)), { total: 60, fadeOut: 0.45 });
  const fade = texts[0].animations.find((a) => a.type === 'fade');
  assert.ok(fade.fade_out_at >= Math.max(...texts[0].char_times) + 0.15 - 1e-9);
  const next = texts[1].animations.find((a) => a.type === 'fade');
  assert.ok(fade.fade_out_at <= next.start_at, 'and still clears the next cue');
});

test('scenes/bird.lrc keeps every word lit until the next line', () => {
  const src = fs.readFileSync(new URL('../scenes/bird.lrc', import.meta.url), 'utf8');
  const texts = buildLyricTexts(applyOffset(parseLrc(src)), {
    total: 367,
    entrance: 'slide',
    jitter: { amplitude: 0.035 },
    wave: { amplitude: 0.09 },
  });
  assert.equal(texts.length, 33);
  for (let i = 0; i < texts.length; i++) {
    const t = texts[i];
    assert.ok(t.char_times, `line ${i} lost its word timing`);
    assert.equal(t.char_times.length, t.text.length, `line ${i} glyph count`);
    for (let k = 1; k < t.char_times.length; k++) {
      assert.ok(t.char_times[k] >= t.char_times[k - 1], `line ${i} stamps run backwards`);
    }
    const fade = t.animations.find((a) => a.type === 'fade');
    assert.ok(
      fade.fade_out_at >= Math.max(...t.char_times),
      `line ${i} fades out over its own tail`,
    );
    if (i + 1 < texts.length) {
      const next = texts[i + 1].animations.find((a) => a.type === 'fade');
      assert.ok(fade.fade_out_at <= next.start_at, `line ${i} overlaps the next cue`);
    }
  }
});
