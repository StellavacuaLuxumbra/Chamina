// Region mean-luminance ratio between two PNGs — the C++/Rust regression gate
// (docs/HANDOFF.md §2: ratios must stay within 0.97–1.03; drift > 0.05 is a bug).
// Pixel-level comparison is meaningless (fbm sin differs across f32/GPU/f64),
// only region statistics are compared.
//
//   node js/region-cmp.mjs <test.png> <gold.png>
//
// Regions (fraction of frame): sky = top 20%, horizon = rows 60–75%,
// text = central band rows 38–62% × cols 30–70%, full = everything.

import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const ffmpeg = require('@ffmpeg-installer/ffmpeg').path;

const [, , a, b] = process.argv;
if (!a || !b) {
  console.error('usage: node js/region-cmp.mjs <test.png> <gold.png>');
  process.exit(1);
}

function size(p) {
  let out = '';
  try {
    execFileSync(ffmpeg, ['-hide_banner', '-i', p],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (e) {
    out = e.stderr || '';
  }
  const line = out.split('\n').find((l) => l.includes('Video:'));
  const m = line && line.match(/ (\d{2,5})x(\d{2,5})/);
  if (!m) {
    console.error('cannot probe size of', p);
    process.exit(1);
  }
  return { w: +m[1], h: +m[2] };
}

const sa = size(a), sb = size(b);
if (sa.w !== sb.w || sa.h !== sb.h) {
  console.error('size mismatch', sa, sb);
  process.exit(1);
}
const raw = (p) => execFileSync(ffmpeg,
  ['-hide_banner', '-loglevel', 'error', '-i', p, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
  { maxBuffer: 1 << 30, encoding: 'buffer' });
const ra = raw(a), rb = raw(b);
const W = sa.w, H = sa.h;

const luma = (rawv, x, y) => {
  const o = (y * W + x) * 3;
  return 0.299 * rawv[o] + 0.587 * rawv[o + 1] + 0.114 * rawv[o + 2];
};

function regionMean(rawv, y0f, y1f, x0f, x1f) {
  const y0 = Math.floor(y0f * H), y1 = Math.floor(y1f * H);
  const x0 = Math.floor(x0f * W), x1 = Math.floor(x1f * W);
  let s = 0, n = 0;
  for (let y = y0; y < y1; y += 2)
    for (let x = x0; x < x1; x += 2) { s += luma(rawv, x, y); n++; }
  return s / n;
}

const regions = {
  sky: [0.0, 0.20, 0.0, 1.0],
  horizon: [0.60, 0.75, 0.0, 1.0],
  text: [0.38, 0.62, 0.30, 0.70],
  full: [0.0, 1.0, 0.0, 1.0],
};
let bad = false;
for (const [name, r] of Object.entries(regions)) {
  const ma = regionMean(ra, r[0], r[1], r[2], r[3]);
  const mb = regionMean(rb, r[0], r[1], r[2], r[3]);
  const ratio = ma / mb;
  if (Math.abs(ratio - 1) > 0.05) bad = true;
  console.log(`${name.padEnd(8)} test=${ma.toFixed(1)} gold=${mb.toFixed(1)} ratio=${ratio.toFixed(3)}`);
}
process.exit(bad ? 2 : 0);
