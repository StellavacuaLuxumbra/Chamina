#!/usr/bin/env node
// chamina frame renderer driver.
//
//   node render.js --data <scene_data.json> --out <out.mp4>
//
// It serves this directory over a local HTTP server (ES modules do not load
// from file://), drives a headless Chromium-based browser one frame at a time
// and pipes every PNG straight into ffmpeg's stdin.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { fileURLToPath } from 'node:url';

import puppeteer from 'puppeteer-core';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

const USAGE = `chamina renderer
  node render.js --data <scene_data.json> --out <out.mp4> [options]

  --data <file>        bridge document written by chamina-render (required)
  --out <file>         output video (required)
  --browser <exe>      browser executable (default: Edge/Chrome autodetect)
  --from <n>           start at frame n (default 0)
  --frames <n>         render at most n frames (debug)
  --still <seconds>    render one frame at t seconds and write a PNG to --out
  --crf <n>            x264 quality, lower is better (default 18)
  --preset <name>      x264 preset (default medium)
  --port <n>           local server port (default: ephemeral)
  --url <query>        extra query string for scene.html (e.g. "debug=1")
  --audio <file>       mux this audio track into the output video
  --gpu                render on the GPU (D3D11) instead of SwiftShader
  --probe <spec>       check that every cue lights up, then exit.
                       spec = "cues", "cues@<sec>" or "1.5,20.2,..."
  -h, --help           show this help`;

function parseArgs(argv) {
  const o = {
    data: null,
    out: null,
    browser: null,
    from: 0,
    frames: null,
    still: null,
    crf: '18',
    preset: 'medium',
    port: 0,
    url: '',
    audio: null,
    gpu: false,
    probe: null,
    help: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => {
      const v = argv[++i];
      if (v === undefined) throw new Error(`${a} needs a value`);
      return v;
    };
    switch (a) {
      case '--data': o.data = next(); break;
      case '--out': o.out = next(); break;
      case '--browser': o.browser = next(); break;
      case '--from': o.from = Number(next()); break;
      case '--frames': o.frames = Number(next()); break;
      case '--still': o.still = next(); break;
      case '--crf': o.crf = next(); break;
      case '--preset': o.preset = next(); break;
      case '--port': o.port = Number(next()); break;
      case '--url': o.url = next(); break;
      case '--audio': o.audio = next(); break;
      case '--gpu': o.gpu = true; break;
      case '--probe': o.probe = next(); break;
      case '-h':
      case '--help': o.help = true; break;
      default: throw new Error(`unknown argument ${a}`);
    }
  }
  return o;
}

// ---------------------------------------------------------------------------
// static server
// ---------------------------------------------------------------------------

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.wasm': 'application/wasm',
  '.map': 'application/json',
};

function startServer(dataPath, port) {
  const server = http.createServer((req, res) => {
    let urlPath;
    try {
      urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    } catch {
      res.writeHead(400).end('bad url');
      return;
    }

    let file;
    if (urlPath === '/favicon.ico') {
      res.writeHead(204).end();
      return;
    } else if (urlPath === '/' || urlPath === '/scene.html') {
      file = path.join(__dirname, 'scene.html');
    } else if (urlPath === '/scene.js') {
      file = path.join(__dirname, 'scene.js');
    } else if (urlPath === '/scene_data.json') {
      file = dataPath;
    } else {
      const target = path.normalize(path.join(__dirname, urlPath));
      const root = path.normalize(__dirname + path.sep);
      if (!target.startsWith(root)) {
        res.writeHead(403).end('forbidden');
        return;
      }
      file = target;
    }

    fs.readFile(file, (err, buf) => {
      if (err) {
        res.writeHead(404).end('not found');
        return;
      }
      res.writeHead(200, {
        'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
        'Cache-Control': 'no-store',
      });
      res.end(buf);
    });
  });

  return new Promise((resolve, reject) => {
    server.on('error', reject);
    server.listen(port, '127.0.0.1', () => resolve(server));
  });
}

// ---------------------------------------------------------------------------
// browser + ffmpeg
// ---------------------------------------------------------------------------

const BROWSER_CANDIDATES = [
  process.env.CHAMINA_BROWSER,
  'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
  process.env.LOCALAPPDATA + '\\Google\\Chrome\\Application\\chrome.exe',
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
  'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
].filter(Boolean);

function findBrowser(explicit) {
  const list = explicit ? [explicit, ...BROWSER_CANDIDATES] : BROWSER_CANDIDATES;
  for (const p of list) {
    if (fs.existsSync(p)) return p;
  }
  throw new Error(
    'no browser found; pass --browser <exe> or set CHAMINA_BROWSER',
  );
}

/**
 * Walk every text block in the bridge document, park the timeline in the
 * middle of its cue and report how many glyphs are actually lit. One browser
 * session serves the whole sweep, so checking thirty-odd lyric lines costs a
 * few seconds instead of a process launch each.
 *
 * `spec` is either `cues` / `cues@<offset>` or an explicit comma list.
 */
async function probeCues(page, data, spec) {
  const duration = data.meta.duration_secs;
  const [mode, off] = String(spec).split('@');
  const offset = Number.isFinite(Number(off)) ? Number(off) : 1.5;

  let samples;
  if (mode === 'cues') {
    samples = data.texts.map((t, i) => {
      const fade = (t.animations || []).find((a) => a.type === 'fade');
      const cue = fade ? (fade.start_at ?? 0) : 0;
      const out = fade && Number.isFinite(fade.fade_out_at) ? fade.fade_out_at : null;
      // Midpoint of the lit window, but never earlier than the entrance sweep.
      const lit = out === null ? cue + Math.max(offset, 1.5)
        : Math.max(cue + Math.max(offset, 1.5), cue + (out - cue) * 0.5);
      return { i, text: t.text, cue, t: Math.min(lit, Math.max(0, duration - 0.2)) };
    });
  } else {
    samples = String(spec).split(',').map((v, i) => ({
      i, text: '', cue: Number(v), t: Number(v),
    }));
  }

  const bad = [];
  for (const s of samples) {
    if (!Number.isFinite(s.t)) continue;
    await page.evaluate((tt) => window.setFrame(tt), s.t);
    const probe = await page.evaluate(() => window.__PROBE__);
    const expected = [...String(s.text)].filter((c) => c.trim()).length;
    const visible = probe?.visible ?? 0;
    const short = expected > 0 && visible < Math.min(expected, Math.ceil(expected * 0.5));
    const mark = visible === 0 || short ? '  <== ' + (visible === 0 ? 'BLANK' : 'partly') : '';
    log(
      `cue ${String(s.i).padStart(2)}  t=${s.t.toFixed(2).padStart(7)}  ` +
        `visible=${String(visible).padStart(3)}/${expected}  ${s.text.slice(0, 14)}${mark}`,
    );
    if (visible === 0 || short) bad.push(s);
  }
  return bad;
}

function findFfmpeg() {
  try {
    const inst = require('@ffmpeg-installer/ffmpeg');
    if (inst.path && fs.existsSync(inst.path)) return inst.path;
  } catch {
    /* fall through to PATH */
  }
  return 'ffmpeg';
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

function log(msg) {
  process.stderr.write(`[render] ${msg}\n`);
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help || !opts.data || (!opts.out && opts.probe === null)) {
    console.log(USAGE);
    process.exit(opts.help ? 0 : 1);
  }

  const dataPath = path.resolve(opts.data);
  const outPath = opts.out ? path.resolve(opts.out) : null;
  const meta = JSON.parse(fs.readFileSync(dataPath, 'utf8')).meta;
  const width = meta.width;
  const height = meta.height;
  const fps = meta.fps;
  const from = Math.max(0, opts.from | 0);
  const end = opts.frames
    ? Math.min(from + opts.frames, meta.total_frames)
    : meta.total_frames;
  const total = Math.max(0, end - from);

  let audioPath = null;
  if (opts.audio) {
    audioPath = path.resolve(opts.audio);
    if (!fs.existsSync(audioPath)) {
      throw new Error(`audio file not found: ${audioPath}`);
    }
  }

  if (outPath) fs.mkdirSync(path.dirname(outPath), { recursive: true });

  const server = await startServer(dataPath, opts.port);
  const port = server.address().port;
  const base = `http://127.0.0.1:${port}`;

  const browserPath = findBrowser(opts.browser);
  const ffmpegPath = findFfmpeg();
  log(`browser: ${browserPath}`);
  log(`ffmpeg : ${ffmpegPath}`);
  log(`frames : ${total} @ ${fps} fps, ${width}x${height}`);

  const browser = await puppeteer.launch({
    executablePath: browserPath,
    headless: true,
    defaultViewport: { width, height, deviceScaleFactor: 1 },
    args: [
      '--no-sandbox',
      '--disable-dev-shm-usage',
      '--hide-scrollbars',
      '--force-device-scale-factor=1',
      '--disable-lcd-text',
      '--ignore-gpu-blocklist',
      ...(opts.gpu
        ? ['--enable-gpu', '--use-angle=d3d11', '--enable-zero-copy']
        : ['--enable-unsafe-swiftshader']),
      '--disable-background-timer-throttling',
      '--disable-renderer-backgrounding',
      `--window-size=${width},${height}`,
    ],
  });

  let ff = null;
  let ffErr = '';
  let failed = false;

  try {
    const page = await browser.newPage();
    page.on('pageerror', (e) => log(`page error: ${e.message}`));
    page.on('console', (m) => {
      if (m.type() === 'error') log(`console: ${m.text()}`);
    });

    const query = opts.url ? (opts.url.startsWith('?') ? opts.url : `?${opts.url}`) : '';
    await page.goto(`${base}/scene.html${query}`, { waitUntil: 'load', timeout: 120000 });
    await page.waitForFunction('window.__READY__ === true', { timeout: 180000 });

    const pageError = await page.evaluate(() => window.__ERROR__ || null);
    if (pageError) throw new Error(`scene failed to build:\n${pageError}`);

    const info = await page.evaluate(() => window.__SCENE_INFO__);
    log(`scene ready: ${info.chars} glyphs ${JSON.stringify(info)}`);

    if (opts.still !== null) {
      const t = Number(opts.still);
      if (!Number.isFinite(t)) throw new Error(`bad --still value: ${opts.still}`);
      await page.evaluate((tt) => window.setFrame(tt), t);
      const probe = await page.evaluate(() => window.__PROBE__);
      log(`probe: ${JSON.stringify(probe)}`);
      const png = await page.screenshot({ type: 'png' });
      fs.writeFileSync(outPath, png);
      log(`still t=${t}s -> ${outPath} (${(png.length / 1024).toFixed(0)} KB)`);
      return;
    }

    if (opts.probe) {
      const data = JSON.parse(fs.readFileSync(dataPath, 'utf8'));
      const bad = await probeCues(page, data, opts.probe);
      log(bad.length ? `probe FAILED on ${bad.length} cue(s)` : 'probe ok — every cue is lit');
      process.exitCode = bad.length ? 1 : 0;
      return;
    }

    // --- ffmpeg --------------------------------------------------------
    const ffArgs = [
      '-y', '-hide_banner', '-loglevel', 'error',
      '-f', 'image2pipe', '-vcodec', 'png',
      '-framerate', String(fps),
      '-i', 'pipe:0',
    ];
    if (audioPath) {
      log(`audio : ${audioPath}`);
      ffArgs.push('-i', audioPath, '-map', '0:v:0', '-map', '1:a:0');
    }
    ffArgs.push(
      '-c:v', 'libx264',
      '-preset', opts.preset,
      '-crf', String(opts.crf),
      '-pix_fmt', 'yuv420p',
      '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2',
    );
    if (audioPath) {
      // pad with silence so the output is exactly as long as the video
      ffArgs.push('-c:a', 'aac', '-b:a', '192k', '-af', 'apad', '-shortest');
    }
    ffArgs.push('-movflags', '+faststart', outPath);
    ff = spawn(ffmpegPath, ffArgs, { stdio: ['pipe', 'ignore', 'pipe'] });
    ff.stderr.on('data', (d) => {
      ffErr = (ffErr + d.toString()).slice(-4000);
    });
    ff.on('error', (e) => {
      failed = true;
      log(`ffmpeg spawn failed: ${e.message}`);
    });
    ff.on('exit', (code, sig) => {
      if (code !== 0 && !failed) {
        failed = true;
        log(`ffmpeg exited with ${code ?? sig}\n${ffErr}`);
      }
    });

    // --- frames --------------------------------------------------------
    const t0 = Date.now();
    for (let i = from; i < end; i++) {
      if (failed) throw new Error('ffmpeg failed');
      const t = i / fps;
      await page.evaluate((tt) => window.setFrame(tt), t);
      const png = await page.screenshot({ type: 'png' });

      if (!ff.stdin.write(png)) await once(ff.stdin, 'drain');

      const done = i - from + 1;
      if (done % 25 === 0 || done === total) {
        const elapsed = (Date.now() - t0) / 1000;
        const rate = done / Math.max(elapsed, 0.001);
        const eta = (total - done) / Math.max(rate, 0.001);
        log(
          `frame ${String(i + 1).padStart(String(end).length)}/${end} ` +
            `${rate.toFixed(2)} fps  elapsed ${elapsed.toFixed(1)}s  eta ${eta.toFixed(0)}s`,
        );
      }
    }

    ff.stdin.end();
    const [code] = await once(ff, 'close');
    if (code !== 0) throw new Error(`ffmpeg failed (${code})\n${ffErr}`);

    const mb = fs.statSync(outPath).size / (1024 * 1024);
    log(`done -> ${outPath} (${mb.toFixed(1)} MB)`);
  } finally {
    if (ff && ff.exitCode === null) ff.kill('SIGKILL');
    await browser.close().catch(() => {});
    server.close();
  }
}

main().catch((err) => {
  log(`ERROR: ${err && err.stack ? err.stack : err}`);
  process.exit(1);
});
