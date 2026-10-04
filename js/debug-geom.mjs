// Geometry diagnostic: draw only the front-facing (z > 0) triangles of a glyph
// so we can see whether the tessellation itself is sane.
//
//   node debug-geom.mjs [charIndex] [textIndex]

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer-core';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const charIndex = Number(process.argv[2] || 0);
const textIndex = Number(process.argv[3] || 0);

const data = JSON.parse(
  fs.readFileSync(path.join(__dirname, '..', 'target', 'chamina', 'scene_data.json'), 'utf8'),
);
const c = data.texts[textIndex].chars[charIndex];
const v = c.vertices;
const idx = c.indices;

const front = [];
const back = [];
for (let t = 0; t < idx.length; t += 3) {
  const a = idx[t], b = idx[t + 1], d = idx[t + 2];
  const zs = [v[a][2], v[b][2], v[d][2]];
  if (zs.every((z) => z > 0)) front.push([a, b, d]);
  else if (zs.every((z) => z < 0)) back.push([a, b, d]);
}

let minx = Infinity, maxx = -Infinity, miny = Infinity, maxy = -Infinity;
for (const p of v) {
  minx = Math.min(minx, p[0]); maxx = Math.max(maxx, p[0]);
  miny = Math.min(miny, p[1]); maxy = Math.max(maxy, p[1]);
}
const pad = 0.1;
const w = maxx - minx + pad * 2;
const h = maxy - miny + pad * 2;
const scale = 600 / Math.max(w, h);
const W = Math.round(w * scale);
const H = Math.round(h * scale);

function polys(tris) {
  return tris
    .map(([a, b, d]) => {
      const pts = [a, b, d]
        .map((i) => {
          const x = (v[i][0] - minx + pad) * scale;
          const y = H - (v[i][1] - miny + pad) * scale;
          return `${x.toFixed(2)},${y.toFixed(2)}`;
        })
        .join(' ');
      return `<polygon points="${pts}" />`;
    })
    .join('\n');
}

const html = `<!doctype html><meta charset="utf-8"><body style="margin:0;background:#111">
<svg width="${W}" height="${H}" xmlns="http://www.w3.org/2000/svg">
  <g fill="#39d0ff" fill-opacity="0.85" stroke="#fff" stroke-width="0.6">${polys(front)}</g>
</svg>
<div style="color:#fff;font:14px monospace;padding:8px">
  ch="${c.ch}" verts=${v.length} tris=${idx.length / 3}
  front=${front.length} back=${back.length}
  bbox=[${minx.toFixed(3)}, ${miny.toFixed(3)}]..[${maxx.toFixed(3)}, ${maxy.toFixed(3)}]
</div>
</body>`;

const outPath = path.join(__dirname, '..', 'target', 'chamina', 'geom.png');
const browser = await puppeteer.launch({
  executablePath:
    process.env.CHAMINA_BROWSER ||
    'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  headless: true,
  defaultViewport: { width: Math.max(W, 700), height: H + 40, deviceScaleFactor: 1 },
});
const page = await browser.newPage();
await page.setContent(html, { waitUntil: 'load' });
const png = await page.screenshot({ type: 'png' });
fs.writeFileSync(outPath, png);
await browser.close();
console.log(`wrote ${outPath}  ch="${c.ch}" verts=${v.length} front=${front.length} back=${back.length}`);
