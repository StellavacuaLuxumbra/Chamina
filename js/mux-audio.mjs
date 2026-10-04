#!/usr/bin/env node
// Merge an audio track into an already-rendered chamina video, without
// re-rendering anything.
//
//   node js/mux-audio.mjs <video.mp4> <audio.(mp3|wav|m4a|...)> [-o out.mp4]
//
// Video stream is copied bit-for-bit; only the audio is (re)encoded to AAC.
// If the audio is shorter than the video the tail is padded with silence; if
// it is longer the output stops at the end of the video.

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';

const require = createRequire(import.meta.url);

const USAGE = `mux-audio
  node js/mux-audio.mjs <video.mp4> <audio-file> [-o <out.mp4>] [--offset <sec>]

  --offset <sec>   shift the audio by this many seconds (+ = earlier, - = later)
  -h, --help       show this help`;

const argv = process.argv.slice(2);
if (!argv.length || argv.includes('-h') || argv.includes('--help')) {
  console.log(USAGE);
  process.exit(argv.length ? 0 : 1);
}

const pos = [];
let offset = 0;
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--offset') {
    offset = Number(argv[++i]);
    if (!Number.isFinite(offset)) throw new Error('--offset needs a number');
  } else if (a === '-o') {
    pos.push(argv[++i]);
  } else if (a.startsWith('-')) {
    throw new Error(`unknown option ${a}`);
  } else {
    pos.push(a);
  }
}

const [video, audio, out] = pos;
if (!video || !audio || !out) {
  console.error(USAGE);
  process.exit(1);
}
for (const f of [video, audio]) {
  if (!fs.existsSync(f)) throw new Error(`not found: ${f}`);
}

const ffmpeg = require('@ffmpeg-installer/ffmpeg').path;
fs.mkdirSync(path.dirname(path.resolve(out)), { recursive: true });

// Positive offset = the audio should start `offset`s earlier (trim its head);
// negative = it should start later (delay it).
const args = ['-y', '-hide_banner', '-loglevel', 'error', '-i', video];
if (offset > 0) args.push('-ss', String(offset));
args.push('-i', audio, '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k');

const filters = [];
if (offset < 0) filters.push(`adelay=${Math.round(-offset * 1000)}`);
// apad makes the audio stream unbounded so -shortest cuts at the video end
filters.push('apad');
args.push('-af', filters.join(','), '-shortest', '-movflags', '+faststart', out);

console.log(`mux-audio: ${path.basename(video)} + ${path.basename(audio)} -> ${out}`);
const r = spawnSync(ffmpeg, args, { stdio: 'inherit' });
if (r.status !== 0) process.exit(r.status ?? 1);

const mb = fs.statSync(out).size / (1024 * 1024);
console.log(`done -> ${out} (${mb.toFixed(1)} MB)`);
