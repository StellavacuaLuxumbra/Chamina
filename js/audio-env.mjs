// Offline audio analysis for the outro visualiser: turns the soundtrack into
// a per-frame frequency envelope the browser scene can pump a spectrum ring
// with. The renderer never reads audio, so this runs ahead of time and the
// result rides to the page as a plain JSON file.
//
//   node js/audio-env.mjs <audio.(mp3|wav|m4a|...)> [-o out.json] [--fps 60]
//                         [--bands 64] [--lo 40] [--hi 14000]
//
// Output: { fps, bands, frames, lo, hi, data: [[...band energies 0..1] ...] }
// Energies are band magnitudes mapped from a fixed -60..0 dB window, so the
// same track always produces the same file.

import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';

const require_ = createRequire(import.meta.url);

const USAGE = `audio-env
node js/audio-env.mjs <audio-file> [-o out.json] [--fps 60] [--bands 64] [--lo 40] [--hi 14000]`;

function parseArgs(argv) {
  const o = { out: null, fps: 60, bands: 64, lo: 40, hi: 14000 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-o') o.out = argv[++i];
    else if (a === '--fps') o.fps = Number(argv[++i]);
    else if (a === '--bands') o.bands = Number(argv[++i]);
    else if (a === '--lo') o.lo = Number(argv[++i]);
    else if (a === '--hi') o.hi = Number(argv[++i]);
    else if (a === '-h' || a === '--help') { console.log(USAGE); process.exit(0); }
    else if (a.startsWith('-')) throw new Error(`unknown flag ${a}`);
    else o.input = a;
  }
  if (!o.input) { console.log(USAGE); process.exit(1); }
  return o;
}

function findFfmpeg() {
  try { return require_('@ffmpeg-installer/ffmpeg').path; } catch { return 'ffmpeg'; }
}

/** Decode to mono f32 at `rate`; resolves { samples: Float32Array, rate }. */
function decode(input, rate) {
  const ff = findFfmpeg();
  const args = [
    '-hide_banner', '-loglevel', 'error', '-i', input,
    '-f', 'f32le', '-acodec', 'pcm_f32le', '-ac', '1', '-ar', String(rate), 'pipe:1',
  ];
  return new Promise((resolve, reject) => {
    const ffproc = spawn(ff, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    const chunks = [];
    let err = '';
    ffproc.stdout.on('data', (d) => chunks.push(d));
    ffproc.stderr.on('data', (d) => { err = (err + d.toString()).slice(-2000); });
    ffproc.on('error', reject);
    ffproc.on('exit', (code) => {
      if (code !== 0) { reject(new Error(`ffmpeg exited ${code}: ${err}`)); return; }
      const buf = Buffer.concat(chunks);
      const n = buf.length >> 2;
      const samples = new Float32Array(n);
      for (let i = 0; i < n; i++) samples[i] = buf.readFloatLE(i * 4);
      resolve({ samples, rate });
    });
  });
}

/** In-place iterative radix-2 FFT over re/im of length 2^n. */
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wr = Math.cos(ang);
    const wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1;
      let ci = 0;
      for (let k = 0; k < len >> 1; k++) {
        const j = i + k;
        const l = j + (len >> 1);
        const tr = re[l] * cr - im[l] * ci;
        const ti = re[l] * ci + im[l] * cr;
        re[l] = re[j] - tr;
        im[l] = im[j] - ti;
        re[j] += tr;
        im[j] += ti;
        const nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const RATE = 44100;
  const { samples, rate } = await decode(opts.input, RATE);
  if (rate !== RATE) throw new Error(`decoded at ${rate}, expected ${RATE}`);

  const fps = opts.fps;
  const hop = rate / fps; // samples per frame
  const N = 2048;
  const frames = Math.max(0, Math.floor((samples.length - N) / hop));
  if (!frames) throw new Error(`audio too short (${samples.length} samples)`);

  // Log-spaced band edges across [lo, hi].
  const B = opts.bands;
  const edges = [];
  for (let b = 0; b <= B; b++) {
    edges.push(Math.round(opts.lo * Math.pow(opts.hi / opts.lo, b / B)));
  }
  const binHz = RATE / N;
  const bandBin = edges.map((hz) => Math.min(N >> 1, Math.max(1, Math.round(hz / binHz))));

  // Hann window, precomputed.
  const win = new Float32Array(N);
  for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (N - 1));

  const re = new Float32Array(N);
  const im = new Float32Array(N);
  const DB_LO = -60;
  const data = [];
  for (let f = 0; f < frames; f++) {
    const off = Math.round(f * hop);
    let rms = 0;
    for (let i = 0; i < N; i++) {
      const s = samples[off + i] * win[i];
      re[i] = s;
      im[i] = 0;
      rms += s * s;
    }
    rms = Math.sqrt(rms / N);
    fft(re, im);
    const row = new Array(B);
    for (let b = 0; b < B; b++) {
      let sum = 0;
      const lo = bandBin[b];
      const hi = Math.max(lo + 1, bandBin[b + 1]);
      for (let k = lo; k < hi; k++) sum += re[k] * re[k] + im[k] * im[k];
      // Divide by N/2 (Hann coherent gain) so a full-scale sine peaks at 0 dB;
      // without this every band of a loud master saturates the -60..0 window.
      const mag = Math.sqrt(sum / (hi - lo)) / (N / 2);
      const db = 20 * Math.log10(mag + 1e-9);
      row[b] = Math.round(Math.max(0, Math.min(1, (db - DB_LO) / -DB_LO)) * 1000) / 1000;
    }
    void rms;
    data.push(row);
  }

  const out = { fps, bands: B, frames, lo: opts.lo, hi: opts.hi, data };
  const outPath = opts.out || 'target/chamina/audio_env.json';
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify(out));
  console.log(`audio-env: ${outPath}  frames=${frames} bands=${B} ${fps}fps`);
}

main().catch((e) => { console.error(e.message); process.exit(1); });
