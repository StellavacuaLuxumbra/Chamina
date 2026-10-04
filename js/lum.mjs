// Luminance ASCII preview of an image — a stand-in for actually looking at a
// render. Decodes via ffmpeg and maps local mean luma onto a ramp, so layout,
// contrast and glow falloff are readable straight from the terminal.
//
//   node js/lum.mjs <image.png> [cols] [rows] [gamma]

import { execFileSync, spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const ffmpeg = require('@ffmpeg-installer/ffmpeg').path;

const [, , input, colsArg, rowsArg, gammaArg] = process.argv;
if (!input) {
  console.error('usage: node js/lum.mjs <image> [cols] [rows] [gamma]');
  process.exit(1);
}
const cols = Number(colsArg || 176);
const rows = Number(rowsArg || 48);
const gamma = Number(gammaArg || 0.6);

const probe = spawnSync(ffmpeg, ['-hide_banner', '-i', input], { encoding: 'utf8' });
const info = (probe.stderr || '').split('\n').find((l) => l.includes('Video:'));
const m = info.match(/ (\d{2,5})x(\d{2,5})/);
const W = Number(m[1]);
const H = Number(m[2]);

const raw = execFileSync(
  ffmpeg,
  ['-hide_banner', '-loglevel', 'error', '-i', input, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
  { maxBuffer: 1 << 30, encoding: 'buffer' },
);

const ramp = ' .:-=+*#%@';
const out = [];
for (let cy = 0; cy < rows; cy++) {
  let line = '';
  for (let cx = 0; cx < cols; cx++) {
    const x0 = Math.floor((cx * W) / cols);
    const x1 = Math.max(x0 + 1, Math.floor(((cx + 1) * W) / cols));
    const y0 = Math.floor((cy * H) / rows);
    const y1 = Math.max(y0 + 1, Math.floor(((cy + 1) * H) / rows));
    let s = 0;
    let n = 0;
    for (let y = y0; y < y1; y += 2) {
      for (let x = x0; x < x1; x += 2) {
        const o = (y * W + x) * 3;
        s += 0.299 * raw[o] + 0.587 * raw[o + 1] + 0.114 * raw[o + 2];
        n++;
      }
    }
    const v = s / n;
    line += ramp[Math.min(9, Math.floor(Math.pow(v / 255, gamma) * 10))];
  }
  out.push(line);
}
console.log(`${input}  ${W}x${H}  ->  ${cols}x${rows}  gamma=${gamma}`);
console.log(out.join('\n'));
