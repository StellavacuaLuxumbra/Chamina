import test from 'node:test';
import assert from 'node:assert/strict';

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
