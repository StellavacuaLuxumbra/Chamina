// Load the editor page headlessly and report any script errors.
//
//   node js/editor-check.mjs [--port 7314] [--shot editor.png]
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

import puppeteer from 'puppeteer-core';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
};
const PORT = opt('--port', '7314');
const SHOT = opt('--shot', path.join(ROOT, 'target', 'chamina', 'editor.png'));

function waitFor(fn, ms, what) {
  const t0 = Date.now();
  return new Promise((resolve, reject) => {
    const tick = async () => {
      try {
        if (await fn()) return resolve();
      } catch { /* keep polling */ }
      if (Date.now() - t0 > ms) return reject(new Error(`timeout waiting for ${what}`));
      setTimeout(tick, 200);
    };
    tick();
  });
}

async function main() {
  const server = spawn('node', [path.join(__dirname, 'editor.js'), '--port', PORT], {
    cwd: ROOT,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  server.stdout.on('data', (d) => process.stdout.write(d));
  server.stderr.on('data', (d) => process.stderr.write(d));

  const browser = await puppeteer.launch({
    executablePath: 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    headless: true,
    defaultViewport: { width: 1600, height: 1000, deviceScaleFactor: 1 },
    args: ['--no-sandbox', '--disable-dev-shm-usage', '--enable-unsafe-swiftshader'],
  });

  const problems = [];
  try {
    const page = await browser.newPage();
    page.on('pageerror', (e) => problems.push(`pageerror: ${e.message}`));
    page.on('console', (m) => {
      if (m.type() === 'error') problems.push(`console: ${m.text()}`);
    });

    await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: 'load', timeout: 60000 });
    await waitFor(
      () => page.evaluate(() => document.getElementById('sceneSel').options.length > 0),
      15000,
      'the scene list',
    );
    await waitFor(
      () => page.evaluate(() => document.getElementById('json').value.length > 20),
      15000,
      'scene JSON to load',
    );

    const info = await page.evaluate(() => ({
      scenes: [...document.getElementById('sceneSel').options].map((o) => o.value),
      scene: document.getElementById('sceneSel').value,
      jsonBytes: document.getElementById('json').value.length,
      duration: JSON.parse(document.getElementById('json').value).duration,
      lines: JSON.parse(document.getElementById('json').value).texts.length,
      frame: !!document.getElementById('preview').contentWindow,
    }));
    console.log('editor state:', JSON.stringify(info));

    // The soundtrack picker has to see whatever audio is sitting in the repo.
    const audio = await page
      .evaluate(async () => {
        const r = await fetch('/api/audio');
        return r.ok ? r.json() : [];
      })
      .catch(() => []);
    console.log('audio list:', audio.map((a) => a.name).join(', ') || '(none)');
    if (!audio.length) problems.push('/api/audio returned no soundtrack candidates');

    // Opening a scene exports it first, then swaps the iframe, so the check has
    // to wait for the *rebuilt* preview — the one present at load time is the
    // previous export. Glyph count is the handshake: it only matches once the
    // Rust exporter has written scene_data.json and scene.js has consumed it.
    const expected = await page.evaluate(() => {
      const s = JSON.parse(document.getElementById('json').value);
      return s.texts.reduce(
        (n, t) => n + [...String(t.text)].filter((c) => c.trim()).length,
        0,
      );
    });
    let preview = null;
    // Gate on the exact count: the iframe still showing the previous export
    // has glyphs too, so "> 0" would pass before the new one arrives.
    await waitFor(() => page.evaluate((exp) => {
      const w = document.getElementById('preview').contentWindow;
      return !!(w && w.__SCENE_INFO__ && w.__SCENE_INFO__.chars === exp
                && typeof w.setFrame === 'function');
    }, expected), 90000, 'the preview scene to build');
    preview = await page.evaluate(() => {
      const w = document.getElementById('preview').contentWindow;
      return w.__SCENE_INFO__ ? { chars: w.__SCENE_INFO__.chars } : null;
    });
    console.log(`preview ready: ${JSON.stringify(preview)}  expected glyphs: ${expected}`);
    if (!preview || preview.chars !== expected) {
      problems.push(`preview has ${preview?.chars} glyphs, scene needs ${expected}`);
    }

    // Scrub to the first lyric cue and confirm the frame actually draws. The
    // cue is read from the scene spec rather than hardcoded, so a scene that
    // starts at 0s and one that starts at 16s both exercise the same path.
    const cue = await page.evaluate(() => {
      let spec = null;
      try { spec = JSON.parse(document.getElementById('json').value); } catch { return 10.2; }
      const cues = [];
      for (const t of spec.texts || []) {
        for (const a of t.animations || []) {
          if (a.type === 'fade' || a.type === 'slide' || a.type === 'fall') {
            cues.push(a.start_at ?? 0);
          }
        }
      }
      return cues.length ? Math.min(...cues) : 10.2;
    });
    const probeAt = Math.min(cue + 1.5, Math.max(0, (await page.evaluate(() => {
      const t = document.getElementById('json').value;
      try { return JSON.parse(t).duration || 7; } catch { return 7; }
    })) - 0.2));
    await page.evaluate(async (t) => {
      const scrub = document.getElementById('scrub');
      scrub.value = String(t);
      scrub.dispatchEvent(new Event('input'));
    }, probeAt);
    await new Promise((r) => setTimeout(r, 1200));
    // The iframe may still be swapping documents when the scrubber fires, so
    // poll for scene.js's setFrame hook rather than assuming it is installed.
    await waitFor(() => page.evaluate(() => {
      const w = document.getElementById('preview').contentWindow;
      return !!(w && typeof w.setFrame === 'function' && w.__SCENE_INFO__);
    }), 30000, 'scene.js setFrame()');
    const probe = await page.evaluate(async (t) => {
      const w = document.getElementById('preview').contentWindow;
      await w.setFrame(t);
      return w.__PROBE__;
    }, probeAt);
    console.log(`probe at t=${probeAt.toFixed(2)}s (first cue ${cue}s):`, JSON.stringify({
      t: probe?.t,
      camera: probe?.camera,
      visible: probe?.visible,
      litSample: probe?.litSample,
      glCenter: probe?.gl?.center,
    }));
    if (!(probe?.visible > 0)) {
      problems.push(
        `no glyph is visible at t=${probeAt.toFixed(2)}s, where a lyric line should be on cue`,
      );
    }

    fs.mkdirSync(path.dirname(SHOT), { recursive: true });
    await page.screenshot({ path: SHOT });
    console.log(`shot -> ${SHOT}`);
  } finally {
    await browser.close().catch(() => {});
    server.kill();
  }

  if (problems.length) {
    console.log(`PROBLEMS (${problems.length}):`);
    for (const p of problems) console.log('  ' + p);
    process.exitCode = 1;
  } else {
    console.log('no page errors');
  }
}

main().catch((e) => {
  console.error(String(e && e.stack ? e.stack : e));
  process.exit(1);
});
