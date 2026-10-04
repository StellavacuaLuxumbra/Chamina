// ASCII preview of a PNG, so renders can be inspected without trusting the
// image viewer. Decodes via ffmpeg into raw RGB and prints a colour-class map.
//
//   node js/preview.mjs <image.png> [cols] [rows]

import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const ffmpeg = require('@ffmpeg-installer/ffmpeg').path;

const [, , input, colsArg, rowsArg] = process.argv;
if (!input) {
  console.error('usage: node js/preview.mjs <image> [cols] [rows]');
  process.exit(1);
}
const cols = Number(colsArg || 96);
const rows = Number(rowsArg || 30);

const probe = spawnSync(ffmpeg, ['-hide_banner', '-i', input], { encoding: 'utf8' });
const info = (probe.stderr || '')
  .split('\n')
  .find((l) => l.includes('Video:'));
const m = info.match(/ (\d{2,5})x(\d{2,5})/);
const W = Number(m[1]);
const H = Number(m[2]);

const raw = execFileSync(
  ffmpeg,
  ['-hide_banner', '-loglevel', 'error', '-i', input, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
  { maxBuffer: 1 << 30, encoding: 'buffer' },
);

// colour classes
function classify(r, g, b) {
  const mx = Math.max(r, g, b);
  const mn = Math.min(r, g, b);
  const sat = mx - mn;
  if (mx < 40) return ' ';
  if (sat < 30) return mx > 200 ? '#' : '.';
  // hue
  if (g > mx * 0.7 && b > mx * 0.7) return 'c'; // cyan-ish (text / particles)
  if (r > mx * 0.7 && b > mx * 0.55 && g < mx * 0.7) return 'p'; // pink
  if (r > mx * 0.85 && g > mx * 0.7 && b < mx * 0.7) return 'y'; // gold/yellow
  if (g > mx * 0.8 && r < mx * 0.7) return 'G'; // green (test box)
  if (r > mx * 0.8) return 'r';
  return '?';
}

const out = [];
const gw = cols;
const gh = rows;
for (let cy = 0; cy < gh; cy++) {
  let line = '';
  for (let cx = 0; cx < gw; cx++) {
    const x0 = Math.floor((cx * W) / gw);
    const x1 = Math.max(x0 + 1, Math.floor(((cx + 1) * W) / gw));
    const y0 = Math.floor((cy * H) / gh);
    const y1 = Math.max(y0 + 1, Math.floor(((cy + 1) * H) / gh));
    const counts = {};
    for (let y = y0; y < y1; y += 2) {
      for (let x = x0; x < x1; x += 2) {
        const o = (y * W + x) * 3;
        const k = classify(raw[o], raw[o + 1], raw[o + 2]);
        counts[k] = (counts[k] || 0) + 1;
      }
    }
    // prefer the most "interesting" class present in the cell
    const order = ['G', 'p', 'y', 'c', 'r', 'r', '#', '?', '.', ' '];
    let best = ' ';
    let bestScore = -1;
    for (const [k, n] of Object.entries(counts)) {
      const score = n * (order.indexOf(k) >= 0 && order.indexOf(k) < 7 ? 10 : 1);
      if (score > bestScore) {
        bestScore = score;
        best = k;
      }
    }
    line += best;
  }
  out.push(line);
}
console.log(`${input}  ${W}x${H}  ->  ${gw}x${gh}`);
console.log(out.join('\n'));
