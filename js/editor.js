#!/usr/bin/env node
// chamina local editor.
//
//   node js/editor.js [--port 7311] [--root <dir>]
//
// A small loopback-only server that backs the browser editor: it lists and
// saves scene JSON under `scenes/`, runs the Rust exporter for previews and
// drives a full render while streaming its progress back to the page. The
// browser does the LRC parsing and JSON editing itself (it imports ./lrc.js),
// so the server only ever moves files and child processes around.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const SCENES = path.join(ROOT, 'scenes');
const EXE = 'D:\\ChaminaTarget\\x86_64-pc-windows-gnu\\debug\\chamina-render.exe';

const USAGE = `chamina editor
  node js/editor.js [options]

  --port <n>   listen port (default 7311)
  --exe <path> chamina-render binary (default the debug build)
  -h, --help`;

function parseArgs(argv) {
  const o = { port: 7311, exe: null, help: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') o.port = Number(argv[++i]);
    else if (a === '--exe') o.exe = argv[++i];
    else if (a === '-h' || a === '--help') o.help = true;
    else throw new Error(`unknown argument ${a}`);
  }
  return o;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.woff2': 'font/woff2',
};

function send(res, status, body, type = 'application/json; charset=utf-8') {
  res.writeHead(status, { 'content-type': type, 'cache-control': 'no-store' });
  res.end(body);
}

function sendJson(res, status, obj) {
  send(res, status, JSON.stringify(obj));
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > 32 * 1024 * 1024) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

/** Scene names are plain file stems; refuse anything that could escape. */
function scenePath(name) {
  const stem = String(name || '').replace(/\.json$/i, '');
  if (!/^[\w\u4e00-\u9fff-]{1,80}$/.test(stem)) throw new Error(`bad scene name: ${name}`);
  return path.join(SCENES, `${stem}.json`);
}

function listScenes() {
  if (!fs.existsSync(SCENES)) return [];
  return fs
    .readdirSync(SCENES)
    .filter((f) => f.endsWith('.json'))
    .map((f) => {
      const st = fs.statSync(path.join(SCENES, f));
      return { name: f.replace(/\.json$/i, ''), size: st.size, mtime: st.mtimeMs };
    })
    .sort((a, b) => b.mtime - a.mtime);
}

const AUDIO_EXT = /\.(mp3|wav|m4a|aac|flac|ogg|opus)$/i;

/** Audio tracks the muxer may use: the repo root plus `music/`. */
function listAudio() {
  const seen = new Set();
  const out = [];
  for (const dir of [ROOT, path.join(ROOT, 'music')]) {
    if (!fs.existsSync(dir)) continue;
    for (const f of fs.readdirSync(dir)) {
      if (!AUDIO_EXT.test(f)) continue;
      const abs = path.join(dir, f);
      if (seen.has(abs) || !fs.statSync(abs).isFile()) continue;
      seen.add(abs);
      out.push({
        name: f,
        rel: path.relative(ROOT, abs).split(path.sep).join('/'),
        size: fs.statSync(abs).size,
      });
    }
  }
  return out.sort((a, b) => b.size - a.size);
}

/** Resolve a client-supplied audio path, refusing anything outside the repo. */
function audioPath(rel) {
  const abs = path.resolve(ROOT, String(rel || ''));
  if (!abs.startsWith(path.normalize(ROOT + path.sep))) {
    throw new Error(`bad audio path: ${rel}`);
  }
  if (!fs.existsSync(abs)) throw new Error(`no such audio: ${rel}`);
  return abs;
}

function runExe(exe, argv, onLine) {
  return new Promise((resolve, reject) => {
    const child = spawn(exe, argv, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
    const buffers = { stdout: '', stderr: '' };
    // Children emit \r-separated progress as well as \n, so split on both.
    const pump = (key, chunk) => {
      buffers[key] += chunk.toString('utf8');
      const parts = buffers[key].split(/\r?\n|\r/);
      buffers[key] = parts.pop() ?? '';
      for (const line of parts) if (line.trim() && onLine) onLine(line);
    };
    child.stdout.on('data', (d) => pump('stdout', d));
    child.stderr.on('data', (d) => pump('stderr', d));
    child.on('error', reject);
    child.on('close', (code) => resolve({ code, ...buffers }));
  });
}

// ---------------------------------------------------------------------------
// render job — one at a time, progress pushed to whoever is listening
// ---------------------------------------------------------------------------

const job = { running: false, lines: [], listeners: new Set(), last: null };

function pushLine(line) {
  job.lines.push(line);
  if (job.lines.length > 400) job.lines.shift();
  for (const fn of job.listeners) fn(line);
}

async function startRender(args, name) {
  if (job.running) throw new Error('a render is already running');
  job.running = true;
  job.lines = [];
  pushLine(`[editor] rendering ${name} ...`);
  try {
    const res = await runExe(args.exe, args.argv, pushLine);
    job.last = { code: res.code, out: args.argv.join(' ') };
    pushLine(res.code === 0 ? '[editor] done' : `[editor] failed (exit ${res.code})`);
    if (res.code !== 0) job.last.error = res.stderr.slice(-4000);
    return { code: res.code, ...job.last };
  } finally {
    job.running = false;
  }
}

// ---------------------------------------------------------------------------
// static + api
// ---------------------------------------------------------------------------

function serveStatic(req, res, urlPath) {
  if (urlPath === '/favicon.ico') { send(res, 204, Buffer.alloc(0), 'image/x-icon'); return true; }
  const routes = {
    '/': 'editor.html',
    '/editor.html': 'editor.html',
    '/scene.html': 'scene.html',
    '/scene.js': 'scene.js',
    // scene.js pulls the shared animation math from here; without this entry
    // the preview iframe fails to import and never reports "scene built".
    '/anim.mjs': 'anim.mjs',
    '/lrc.js': 'lrc.js',
    '/preview.mjs': 'preview.mjs',
  };
  let file;
  if (urlPath === '/scene_data.json') {
    file = path.join(ROOT, 'target', 'chamina', 'scene_data.json');
  } else if (routes[urlPath]) {
    file = path.join(__dirname, routes[urlPath]);
  } else if (urlPath.startsWith('/node_modules/')) {
    // Only the browser build artifacts are reachable — no arbitrary reads.
    const rel = path.normalize(urlPath.slice(1)).replace(/^(\.\.[/\\])+/, '');
    const abs = path.join(__dirname, rel);
    if (!abs.startsWith(__dirname + path.sep)) {
      send(res, 403, 'forbidden', 'text/plain');
      return true;
    }
    file = abs;
  } else {
    return false;
  }
  if (!fs.existsSync(file)) {
    send(res, 404, 'not found', 'text/plain');
    return true;
  }
  const body = fs.readFileSync(file);
  send(res, 200, body, MIME[path.extname(file)] || 'application/octet-stream');
  return true;
}

async function handleApi(req, res, url, defaultExe) {
  const { pathname } = url;
  const q = url.searchParams;

  if (pathname === '/api/scenes') return sendJson(res, 200, listScenes());

  if (pathname === '/api/audio') return sendJson(res, 200, listAudio());

  // Attach a soundtrack to an already-rendered MP4 — no re-render needed.
  if (pathname === '/api/mux' && req.method === 'POST') {
    const stem = String(q.get('name') || '')
      .replace(/\.mp4$/i, '')
      .replace(/[^\w\u4e00-\u9fff-]/g, '');
    if (!stem) return sendJson(res, 400, { error: 'missing name' });
    const audio = audioPath(q.get('audio'));
    const video = path.join(ROOT, 'out', `${stem}.mp4`);
    if (!fs.existsSync(video)) {
      return sendJson(res, 404, { error: `还没有渲染好的 out/${stem}.mp4` });
    }
    const outFile = path.join(ROOT, 'out', `${stem}_music.mp4`);
    const lines = [];
    const r = await runExe(
      process.execPath,
      [path.join(__dirname, 'mux-audio.mjs'), video, audio, '-o', outFile],
      (l) => lines.push(l),
    );
    return sendJson(res, r.code === 0 ? 200 : 500, {
      ok: r.code === 0,
      code: r.code,
      out: r.code === 0 ? path.relative(ROOT, outFile).split(path.sep).join('/') : null,
      lines,
      log: r.stderr.slice(-4000),
    });
  }

  if (pathname === '/api/scene') {
    const file = scenePath(q.get('name'));
    if (req.method === 'GET') {
      if (!fs.existsSync(file)) return sendJson(res, 404, { error: 'no such scene' });
      return send(res, 200, fs.readFileSync(file, 'utf8'), MIME['.json']);
    }
    if (req.method === 'PUT') {
      const body = await readBody(req);
      JSON.parse(body); // reject before touching disk
      fs.mkdirSync(SCENES, { recursive: true });
      fs.writeFileSync(file, body.endsWith('\n') ? body : body + '\n', 'utf8');
      return sendJson(res, 200, { ok: true, name: path.basename(file, '.json') });
    }
  }

  if (pathname === '/api/export' && req.method === 'POST') {
    const file = scenePath(q.get('name'));
    if (!fs.existsSync(file)) return sendJson(res, 404, { error: 'no such scene' });
    const lines = [];
    const out = await runExe(q.get('exe') || defaultExe, [file, '--dry-run'], (l) => lines.push(l));
    return sendJson(res, out.code === 0 ? 200 : 500, {
      ok: out.code === 0,
      code: out.code,
      lines,
      log: out.stderr.slice(-4000),
    });
  }

  // EventSource can only issue GETs, so the stream endpoint accepts both.
  if (pathname === '/api/render' && (req.method === 'GET' || req.method === 'POST')) {
    const name = q.get('name');
    const file = scenePath(name);
    if (!fs.existsSync(file)) return sendJson(res, 404, { error: 'no such scene' });
    const outName = (q.get('out') || `${path.basename(name, '.json')}.mp4`).replace(/[^\w.-]/g, '');
    const outFile = path.join(ROOT, 'out', outName);
    fs.mkdirSync(path.dirname(outFile), { recursive: true });
    // Validate before the SSE headers go out, so a bad path is a plain 500.
    const audio = q.get('audio') ? audioPath(q.get('audio')) : null;
    // Reply first, then stream progress on the same socket.
    res.writeHead(200, {
      'content-type': 'text/event-stream; charset=utf-8',
      'cache-control': 'no-store',
      connection: 'keep-alive',
    });
    const sendLine = (l) => res.write(`data: ${JSON.stringify(l)}\n\n`);
    job.listeners.add(sendLine);
    for (const l of job.lines) sendLine(l);
    res.on('close', () => job.listeners.delete(sendLine));

    const exe = q.get('exe') || defaultExe;
    const argv = [file, '-o', outFile];
    if (audio) argv.push('--audio', audio);
    startRender({ exe, argv }, path.basename(file, '.json')).then(
      (r) => {
        sendLine(JSON.stringify({ done: true, code: r.code, out: path.relative(ROOT, outFile), error: r.error || null }));
        res.end();
      },
      (e) => {
        sendLine(JSON.stringify({ done: true, code: 1, error: String(e.message || e) }));
        res.end();
      },
    );
    return undefined;
  }

  sendJson(res, 404, { error: `no route ${req.method} ${pathname}` });
  return undefined;
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help) {
    console.log(USAGE);
    return;
  }
  const exe = opts.exe || EXE;

  const server = http.createServer(async (req, res) => {
    let url;
    try {
      url = new URL(req.url, 'http://127.0.0.1');
    } catch {
      return send(res, 400, 'bad url', 'text/plain');
    }
    try {
      if (url.pathname.startsWith('/api/')) return await handleApi(req, res, url, exe);
      if (serveStatic(req, res, url.pathname)) return undefined;
      return send(res, 404, 'not found', 'text/plain');
    } catch (err) {
      const message = String(err && err.message ? err.message : err);
      if (!res.headersSent) return sendJson(res, 500, { error: message });
      res.end();
      return undefined;
    }
  });

  server.listen(opts.port, '127.0.0.1', () => {
    const addr = server.address();
    console.log(`chamina editor  http://127.0.0.1:${addr.port}`);
    console.log(`  scenes ${SCENES}`);
    console.log(`  exe    ${exe}`);
  });
}

main();
