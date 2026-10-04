// Sweep bloom intensity and score lyric legibility from a still frame.
//
//   node js/bloom-sweep.mjs <scene_data.json> <t> <v1,v2,...>
//
// Legibility proxy: the standard deviation of luminance along a horizontal
// scanline through the text. Crisp strokes give a large sd; a washed-out glow
// gives a small one.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const FF = path.join(ROOT, 'js', 'node_modules', '@ffmpeg-installer', 'win32-x64', 'ffmpeg.exe');
const NODE = process.execPath;

const [dataFile, tArg, valuesArg] = process.argv.slice(2);
if (!dataFile) {
  console.error('usage: node js/bloom-sweep.mjs <scene_data.json> <t> <v1,v2>');
  process.exit(1);
}
const t = Number(tArg) || 10.2;
const values = (valuesArg || '0.05,0.12,0.3').split(',').map((s) => s.trim()).filter(Boolean);

const base = JSON.parse(fs.readFileSync(dataFile, 'utf8'));
const W = base.meta.width;
const H = base.meta.height;
const SCAN_Y = Math.round(H * 0.648); // through the lyric line

// `0.12@0.8` = intensity 0.12 with blur scale 0.8; a bare number is intensity.
function parseSpec(s) {
  const [i, sc] = String(s).split('@');
  return { intensity: Number(i), scale: sc === undefined ? undefined : Number(sc) };
}

function scanlineStats(binPath) {
  const b = fs.readFileSync(binPath);
  const vals = [];
  for (let x = Math.round(W * 0.31); x < Math.round(W * 0.73); x += 8) {
    const i = (SCAN_Y * W + x) * 3;
    vals.push(Math.max(b[i], b[i + 1], b[i + 2]));
  }
  const mean = vals.reduce((a, c) => a + c, 0) / vals.length;
  const sd = Math.sqrt(vals.reduce((a, c) => a + (c - mean) ** 2, 0) / vals.length);
  return { min: Math.min(...vals), max: Math.max(...vals), mean, sd };
}

const rows = [];
for (const spec of values.map(parseSpec)) {
  const bloom = { ...base.meta.bloom, intensity: spec.intensity };
  if (spec.scale !== undefined) bloom.scale = spec.scale;
  const doc = { ...base, meta: { ...base.meta, bloom } };
  const tag = spec.scale === undefined
    ? String(spec.intensity)
    : `${spec.intensity}@${spec.scale}`;
  const dPath = path.join(ROOT, 'target', 'chamina', `sw_${tag.replace(/[^\w.]/g, '_')}.json`);
  const png = dPath.replace(/\.json$/, '.png');
  const bin = dPath.replace(/\.json$/, '.bin');
  fs.writeFileSync(dPath, JSON.stringify(doc));

  const r = spawnSync(NODE, [
    path.join(ROOT, 'js', 'render.js'),
    '--data', dPath, '--out', png, '--still', String(t),
  ], { cwd: ROOT, stdio: 'pipe' });
  if (r.status !== 0) {
    console.error(`render failed for ${tag}: ${String(r.stderr).slice(0, 400)}`);
    continue;
  }
  spawnSync(FF, ['-v', 'error', '-i', png, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-y', bin]);
  const s = scanlineStats(bin);
  rows.push({ tag, ...s });
  console.log(
    `bloom ${tag.padEnd(10)} min=${String(s.min).padStart(3)} max=${String(s.max).padStart(3)} ` +
      `mean=${s.mean.toFixed(0).padStart(4)} sd=${s.sd.toFixed(1).padStart(6)}`,
  );
}

const best = rows.reduce((a, b) => (b && (!a || b.sd > a.sd) ? b : a), null);
if (best) console.log(`best contrast: bloom ${best.tag} (sd ${best.sd.toFixed(1)})`);
