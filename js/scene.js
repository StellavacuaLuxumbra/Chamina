// chamina headless renderer: builds the Three.js scene from scene_data.json and
// exposes `window.setFrame(t)` which the Puppeteer driver calls once per frame.
//
// Everything here mirrors the arithmetic in the Rust crate (src/animation.rs,
// src/export.rs) — if you change a formula, change it in both places.

import * as THREE from 'three';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { ShaderPass } from 'three/addons/postprocessing/ShaderPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';
import { FXAAShader } from 'three/addons/shaders/FXAAShader.js';
import { BokehPass } from 'three/addons/postprocessing/BokehPass.js';

// Animation arithmetic (easing, per-character curves, jitter noise) lives in
// ./anim.mjs so node can test it without a browser; this file only wires the
// results onto Three.js objects.

import {
  evalAll,
  evalBackgrounds,
  evalOrbit,
  hitEnvelope,
  jitterNoise,
  shakeWeight,
  charDelay,
  smoothstep,
} from './anim.mjs';

// Debug flags (set through `render.js --url "a=1&b=1"`):
//   debug      all of the below at once (head-on camera, no post, no particles)
//   front      fixed head-on camera (ignore camera_animation)
//   nopost     render straight to the canvas (skip EffectComposer)
//   noparticles, nolights, text=<n>   (text=<n> keeps only that line)
//   nooutline    skip the inverted-hull rim (A/B its look)
//   box        add a 3x3x3 green reference cube at the origin (off by default)
//   norings / nobars / nosparks / nostreaks   A/B the audio-visualiser pieces
const QS = new URLSearchParams(typeof location !== 'undefined' ? location.search : '');
const flag = (k) => QS.has(k);
const DEBUG = flag('debug');
const FRONT = DEBUG || flag('front');
const NOPOST = DEBUG || flag('nopost');
const NOPARTICLES = DEBUG || flag('noparticles');
// Audio visualisers (intro ring, outro ring, spectrum bars, cue sparks,
// roll streaks): on when render.js was given --audio-env, which also serves
// /audio_env.json and appends audioenv=1 to the page query.
const AUDIO_ENV = QS.get('audioenv') === '1';
const NORINGS = flag('norings');
const NOBARS = flag('nobars');
const NOSPARKS = flag('nosparks') || NOPARTICLES;
const NOSTREAKS = flag('nostreaks') || NOPARTICLES;
const NOLIGHTS = DEBUG || flag('nolights');
const BOX = DEBUG ? false : flag('box');
const NOOUTLINE = flag('nooutline');
const DEBUG_TEXT = QS.get('text');

// ---------------------------------------------------------------------------
// deterministic randomness (mirrors chamina's seed)
// ---------------------------------------------------------------------------

function mulberry32(seed) {
  let a = seed >>> 0;
  return function () {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// ---------------------------------------------------------------------------
// scene construction
// ---------------------------------------------------------------------------

const state = {
  meta: null,
  scene: null,
  camera: null,
  renderer: null,
  composer: null,
  chars: [],
  particleUniforms: null,
  particles: null,
  orbit: null,
  width: 0,
  height: 0,
  // Background timeline + post rack (both optional, both default off).
  bgUniforms: null,
  bgKeys: [],
  fx: null,
  fxPass: null,
  // Every lyric cue: the moment a line lights up. Drives the flash / split /
  // camera punch so each line lands with weight instead of just fading in.
  cues: [],
  // Filled in by updateCamera so the cue punch can push along the view axis.
  camTarget: [0, 1, 0],
  lastHit: 0,
  // Camera roll this instant (stops + shake), so a landing line's glyphs can
  // follow the bank in and settle level as the camera does.
  camRoll: 0,
  // Mean band energy of the current frame (audio-env scenes only). The cue
  // punch and the flash spend themselves with the music instead of flat out.
  energyNow: 0.75,
  // Audio visualisers built when --audio-env was given.
  env: null,
  viz: null,
};

// scene_data.json stores vertex data as [[x, y, z], ...]; BufferAttribute wants
// a flat typed array, and handing it nested arrays silently produces NaNs.
function toFlat(rows, stride) {
  const out = new Float32Array(rows.length * stride);
  for (let i = 0; i < rows.length; i++) {
    const row = rows[i];
    const o = i * stride;
    for (let j = 0; j < stride; j++) out[o + j] = row[j];
  }
  return out;
}

// The extruder stores caps and side walls as separate vertices that sit on the
// same positions, so a vertex can carry a (0,0,±1) cap normal while its
// twin carries a lateral wall normal. Inflating along those raw normals
// translates walls without ever widening the caps, which drops the rim a whole
// `width` away from the glyph. Averaging normals across coincident positions
// first gives silhouette vertices a diagonal normal, so the hull genuinely
// grows outward in every direction.
function smoothHullGeometry(geo) {
  const pos = geo.attributes.position;
  const nrm = geo.attributes.normal;
  const acc = new Map();
  const keyOf = (i) =>
    `${Math.round(pos.getX(i) * 8192)},${Math.round(pos.getY(i) * 8192)},${Math.round(
      pos.getZ(i) * 8192,
    )}`;
  for (let i = 0; i < pos.count; i++) {
    const k = keyOf(i);
    let e = acc.get(k);
    if (!e) {
      e = [0, 0, 0];
      acc.set(k, e);
    }
    e[0] += nrm.getX(i);
    e[1] += nrm.getY(i);
    e[2] += nrm.getZ(i);
  }
  const out = new Float32Array(pos.count * 3);
  for (let i = 0; i < pos.count; i++) {
    const e = acc.get(keyOf(i));
    const l = Math.hypot(e[0], e[1], e[2]) || 1;
    out[i * 3] = e[0] / l;
    out[i * 3 + 1] = e[1] / l;
    out[i * 3 + 2] = e[2] / l;
  }
  const hull = new THREE.BufferGeometry();
  hull.setAttribute('position', pos);
  hull.setAttribute('normal', new THREE.BufferAttribute(out, 3));
  hull.setIndex(geo.index);
  return hull;
}

function makeMaterial(spec) {
  switch (spec.type) {
    case 'metal': {
      const m = new THREE.MeshStandardMaterial({
        color: new THREE.Color(spec.color),
        metalness: 1.0,
        roughness: spec.roughness,
        // The extrusion is closed on both sides; DoubleSide also makes the
        // renderer immune to fonts whose contours wind the other way.
        side: THREE.DoubleSide,
      });
      m.userData.baseOpacity = 1;
      m.userData.neon = false;
      return m;
    }
    case 'glass': {
      const m = new THREE.MeshStandardMaterial({
        color: new THREE.Color(spec.color),
        metalness: 0.0,
        roughness: spec.roughness,
        transparent: true,
        opacity: spec.opacity ?? 0.25,
        depthWrite: false,
        side: THREE.DoubleSide,
      });
      m.userData.baseOpacity = m.opacity;
      m.userData.neon = false;
      return m;
    }
    case 'neon':
    default: {
      const color = new THREE.Color(spec.color).multiplyScalar(spec.strength ?? 5);
      const m = new THREE.MeshBasicMaterial({
        color,
        side: THREE.DoubleSide,
        // Fade animations write material.opacity; three.js silently ignores
        // opacity on opaque materials, so the neon text would never fade.
        transparent: true,
      });
      m.userData.baseOpacity = 1;
      m.userData.neon = true;
      return m;
    }
  }
}

function buildText(spec, index) {
  const baseRotation = new THREE.Quaternion(
    spec.rotation[0],
    spec.rotation[1],
    spec.rotation[2],
    spec.rotation[3],
  );
  const baseScale = new THREE.Vector3(
    spec.base_scale[0],
    spec.base_scale[1],
    spec.base_scale[2],
  );

  for (const c of spec.chars) {
    const geo = new THREE.BufferGeometry();
    geo.setAttribute(
      'position',
      new THREE.Float32BufferAttribute(toFlat(c.vertices, 3), 3),
    );
    geo.setAttribute(
      'normal',
      new THREE.Float32BufferAttribute(toFlat(c.normals, 3), 3),
    );
    geo.setAttribute('uv', new THREE.Float32BufferAttribute(toFlat(c.uvs, 2), 2));
    geo.setIndex(c.indices);
    geo.computeBoundingSphere();

    // The moment this glyph lights up: the fade's own delay grid, word timing
    // included. Static per glyph, so it is resolved once here for the
    // follow-the-camera roll the renderer applies while a line lands.
    const fadeA = (spec.animations || []).find((a) => a.type === 'fade');
    const landsAt = fadeA
      ? charDelay(spec.char_times || null, fadeA.start_at, fadeA.stagger, c.idx)
      : null;

    const material = makeMaterial(spec.material);
    const mesh = new THREE.Mesh(geo, material);
    mesh.matrixAutoUpdate = true;
    mesh.frustumCulled = false;

    // Inverted-hull rim: push the hull out along its normals and keep only the
    // back faces, so the extra surface shows up as a contour hugging the glyph
    // silhouette. width is in em, so it tracks the text size with the mesh.
    let outlineMaterial = null;
    if (spec.material.outline && !NOOUTLINE) {
      const o = spec.material.outline;
      outlineMaterial = new THREE.ShaderMaterial({
        uniforms: {
          uColor: {
            value: new THREE.Color(o.color).multiplyScalar(o.strength ?? 1),
          },
          uWidth: { value: o.width },
          uOpacity: { value: 1 },
        },
        vertexShader: /* glsl */ `
          uniform float uWidth;
          void main() {
            vec3 p = position + normal * uWidth;
            gl_Position = projectionMatrix * modelViewMatrix * vec4(p, 1.0);
          }
        `,
        fragmentShader: /* glsl */ `
          uniform vec3 uColor;
          uniform float uOpacity;
          void main() {
            if (uOpacity <= 0.002) discard;
            gl_FragColor = vec4(uColor, uOpacity);
          }
        `,
        side: THREE.BackSide,
        transparent: true,
      });
      const hull = new THREE.Mesh(smoothHullGeometry(geo), outlineMaterial);
      hull.frustumCulled = false;
      mesh.add(hull);
    }

    state.scene.add(mesh);
    state.chars.push({
      mesh,
      material,
      outlineMaterial,
      idx: c.idx,
      size: spec.size,
      animations: spec.animations || [],
      // Word timing shared by every animation of this line; `null` for an
      // ordinary LRC, which then reads its delays off the uniform grid.
      charTimes: spec.char_times || null,
      landsAt,
      basePosition: new THREE.Vector3(
        c.base_position[0],
        c.base_position[1],
        c.base_position[2],
      ),
      baseRotation,
      baseScale,
      textAlpha: spec.alpha,
      baseOpacity: material.userData.baseOpacity,
    });
  }
}

function buildLights(lights) {
  const add = (color, intensity, dir) => {
    const l = new THREE.DirectionalLight(
      new THREE.Color(color),
      Math.max(intensity / 1000, 0),
    );
    const n = Math.hypot(dir[0], dir[1], dir[2]) || 1;
    l.position.set((dir[0] / n) * 50, (dir[1] / n) * 50, (dir[2] / n) * 50);
    l.target.position.set(0, 0, 0);
    state.scene.add(l);
    state.scene.add(l.target);
  };
  add(lights.key_color, lights.key_illuminance, lights.key_dir);
  add(lights.rim_color, lights.rim_illuminance, lights.rim_dir);
  add(lights.fill_color, lights.fill_illuminance, lights.fill_dir);

  const amb = new THREE.AmbientLight(
    new THREE.Color(lights.ambient_color),
    Math.max(lights.ambient_brightness / 100, 0),
  );
  state.scene.add(amb);
}

function buildParticles(meta) {
  if (NOPARTICLES) return;
  const N = 2600;
  const rnd = mulberry32(meta.seed >>> 0);

  const offsets = new Float32Array(N * 3);
  const phases = new Float32Array(N);
  const speeds = new Float32Array(N);
  const scales = new Float32Array(N);

  for (let i = 0; i < N; i++) {
    offsets[i * 3 + 0] = (rnd() - 0.5) * 70;
    offsets[i * 3 + 1] = rnd() * 70 - 25;
    // Kept behind the text: the camera orbits at radius ~14, so a cloud that
    // straddles the camera turns near-field points into screen-filling discs.
    offsets[i * 3 + 2] = (rnd() - 0.5) * 45 - 20;
    phases[i] = rnd() * Math.PI * 2;
    speeds[i] = 0.25 + rnd() * 1.4;
    scales[i] = 0.6 + rnd() * 2.4;
  }

  const geo = new THREE.BufferGeometry();
  geo.setAttribute('aOffset', new THREE.BufferAttribute(offsets, 3));
  geo.setAttribute('position', new THREE.BufferAttribute(offsets.slice(), 3));
  geo.setAttribute('aPhase', new THREE.BufferAttribute(phases, 1));
  geo.setAttribute('aSpeed', new THREE.BufferAttribute(speeds, 1));
  geo.setAttribute('aScale', new THREE.BufferAttribute(scales, 1));
  geo.boundingSphere = new THREE.Sphere(new THREE.Vector3(0, 5, 0), 120);

  const uniforms = {
    uTime: { value: 0 },
    uColor: { value: new THREE.Color('#9fd8ff') },
    uOpacity: { value: 0.9 },
  };

  const mat = new THREE.ShaderMaterial({
    uniforms,
    transparent: true,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
    vertexShader: /* glsl */ `
      attribute vec3 aOffset;
      attribute float aPhase;
      attribute float aSpeed;
      attribute float aScale;
      uniform float uTime;
      varying float vTw;
      void main() {
        vec3 p = aOffset;
        p.y = mod(p.y + uTime * aSpeed + 45.0, 70.0) - 25.0;
        vec4 mv = modelViewMatrix * vec4(p, 1.0);
        gl_Position = projectionMatrix * mv;
        // Distance clamp keeps close points from blooming into full-screen discs.
        gl_PointSize = min(aScale * (260.0 / max(-mv.z, 0.001)), 46.0);
        vTw = 0.45 + 0.55 * sin(uTime * 2.1 + aPhase);
        vTw *= smoothstep(2.0, 7.0, -mv.z);
      }
    `,
    fragmentShader: /* glsl */ `
      uniform vec3 uColor;
      uniform float uOpacity;
      varying float vTw;
      void main() {
        float d = length(gl_PointCoord - vec2(0.5));
        float a = smoothstep(0.5, 0.0, d);
        a = a * a * vTw * uOpacity;
        if (a <= 0.001) discard;
        gl_FragColor = vec4(uColor * a * 2.0, a);
      }
    `,
  });

  const points = new THREE.Points(geo, mat);
  points.frustumCulled = false;
  state.scene.add(points);
  state.particleUniforms = uniforms;
  state.particles = points;
}

// ---------------------------------------------------------------------------
// audio visualisers (intro ring, outro ring, spectrum bars, cue sparks, roll
// streaks) — driven per-frame from the precomputed envelope (js/audio-env.mjs)
// ---------------------------------------------------------------------------

/** Window envelope: 0 outside, ramping in over `fadeIn`, out over `fadeOut`. */
function ringWindow(t, w) {
  if (t < w.start || t > w.end) return 0;
  let f = 1;
  if (t < w.start + w.fadeIn) f = (t - w.start) / w.fadeIn;
  const fo = w.end - w.fadeOut;
  if (t > fo) f = Math.min(f, (w.end - t) / w.fadeOut);
  return Math.max(0, Math.min(1, f));
}

/**
 * Spectrum ring: 64 frequency bands laid out clockwise, each a short radial
 * run of points that lights up as far as its energy reaches. v10 colours it
 * with a rainbow that rides the band index and drifts with time, computed in
 * the vertex shader (per point, not per pixel — SwiftShader ground to a halt
 * the last time per-pixel noise entered the chain). The shape never changes,
 * only uniforms, so seeking stays deterministic.
 */
function makeSpectrumRing({
  center,
  r0,
  r1,
  tilt = 0,
  window,
  hueSpeed = 0.045,
  spin = 0.06,
  p0 = 0.9,
  p1 = 2.2,
  opacity = 0.95,
}) {
  const B = 64;
  const RADIAL = 7;
  const N = B * RADIAL;

  // Geometry sits around the local origin; the object's own transform carries
  // it to its place in the world (and its tilt, in the intro ring's case), so
  // the shader's idle spin can keep rotating around the ring's own centre.
  const pos = new Float32Array(N * 3);
  const aBand = new Float32Array(N);
  const aK = new Float32Array(N);
  for (let b = 0; b < B; b++) {
    const ang = -Math.PI / 2 + (b / B) * Math.PI * 2;
    const dx = Math.cos(ang);
    const dy = Math.sin(ang);
    for (let k = 0; k < RADIAL; k++) {
      const i = b * RADIAL + k;
      const r = r0 + ((r1 - r0) * k) / (RADIAL - 1);
      pos[i * 3 + 0] = dx * r;
      pos[i * 3 + 1] = dy * r;
      pos[i * 3 + 2] = 0;
      aBand[i] = b;
      aK[i] = k;
    }
  }

  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
  geo.setAttribute('aBand', new THREE.BufferAttribute(aBand, 1));
  geo.setAttribute('aK', new THREE.BufferAttribute(aK, 1));
  geo.boundingSphere = new THREE.Sphere(new THREE.Vector3(), r1 + 1);

  const uniforms = {
    uBand: { value: new Float32Array(B) },
    uFade: { value: 0 },
    uTime: { value: 0 },
    uHueSpeed: { value: hueSpeed },
    uSpin: { value: spin },
    uP0: { value: p0 },
    uP1: { value: p1 },
    uOpacity: { value: opacity },
  };
  const mat = new THREE.ShaderMaterial({
    uniforms,
    transparent: true,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
    vertexShader: /* glsl */ `
      attribute float aBand;
      attribute float aK;
      uniform float uBand[64];
      uniform float uFade;
      uniform float uTime;
      uniform float uHueSpeed;
      uniform float uSpin;
      uniform float uP0;
      uniform float uP1;
      varying float vGlow;
      varying vec3 vColor;

      vec3 hsv2rgb(vec3 c) {
        vec3 K = vec3(1.0, 2.0 / 3.0, 1.0 / 3.0);
        vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - 3.0);
        return c.z * mix(K.xxx, clamp(p - K.x, 0.0, 1.0), c.y);
      }

      void main() {
        // Slow idle rotation keeps the ring alive once the track has faded.
        float ang = uTime * uSpin;
        float ca = cos(ang);
        float sa = sin(ang);
        vec3 p = vec3(position.x * ca - position.y * sa, position.x * sa + position.y * ca, position.z);
        vec4 mv = modelViewMatrix * vec4(p, 1.0);
        gl_Position = projectionMatrix * mv;
        float e = uBand[int(aBand + 0.5)];
        float lvl = e * 7.0;
        float lit = smoothstep(aK - 0.45, aK + 0.05, lvl);
        // Shimmer gives every point a lazy sine so the quiet tail still moves.
        float shim = 0.5 + 0.5 * sin(uTime * 1.7 + aBand * 1.53);
        float size = mix(uP0, uP1, lit) * (0.75 + 0.45 * e);
        gl_PointSize = min(size * (260.0 / max(-mv.z, 0.001)), 44.0);
        vGlow = 0.16 + 0.84 * lit * (0.72 + 0.28 * shim);
        vGlow *= smoothstep(2.0, 7.0, -mv.z);
        vGlow *= uFade;
        // The rainbow: hue rides the band index and drifts with time, so the
        // ring never shows the same colour twice; lit points run brighter.
        float hue = fract(aBand / 64.0 + uTime * uHueSpeed);
        vColor = hsv2rgb(vec3(hue, 0.85, 0.30 + 0.70 * lit));
      }
    `,
    fragmentShader: /* glsl */ `
      uniform float uOpacity;
      varying float vGlow;
      varying vec3 vColor;
      void main() {
        float d = length(gl_PointCoord - vec2(0.5));
        float a = smoothstep(0.5, 0.0, d);
        a = a * a * vGlow * uOpacity;
        if (a <= 0.001) discard;
        gl_FragColor = vec4(vColor * a * 2.2, a);
      }
    `,
  });

  const points = new THREE.Points(geo, mat);
  points.frustumCulled = false;
  points.position.set(center[0], center[1], center[2]);
  points.rotation.x = tilt;
  points.visible = false;
  state.scene.add(points);
  return { points, uniforms, window };
}

/**
 * 3D spectrum bars: an instanced crown of 64 boxes just outside the outro
 * ring, each stretching with its band and coloured by the same drifting
 * rainbow. The whole crown revolves slowly, so the bars sweep past the
 * closing card like a dial of light.
 */
function makeSpectrumBars({ center, r0 = 6.05, tilt = -0.1, maxLen = 2.6, window }) {
  const B = 64;
  const geo = new THREE.BoxGeometry(1, 1, 1);
  // Origin at the inner end, so `scale.x` grows the bar outwards.
  geo.translate(0.5, 0, 0);
  const mat = new THREE.MeshBasicMaterial({
    transparent: true,
    opacity: 0,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
  });
  const mesh = new THREE.InstancedMesh(geo, mat, B);
  mesh.frustumCulled = false;
  const group = new THREE.Group();
  group.add(mesh);
  group.position.set(center[0], center[1], center[2]);
  group.rotation.x = tilt;
  group.visible = false;
  state.scene.add(group);

  const dummy = new THREE.Object3D();
  const color = new THREE.Color();
  for (let b = 0; b < B; b++) {
    dummy.position.set(0, 0, 0);
    dummy.rotation.set(0, 0, 0);
    dummy.scale.set(0.22, 0.11, 0.11);
    dummy.updateMatrix();
    mesh.setMatrixAt(b, dummy.matrix);
    mesh.setColorAt(b, color.setHSL(b / B, 0.85, 0.4));
  }
  mesh.instanceMatrix.needsUpdate = true;
  if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
  return { mesh, group, r0, maxLen, window, dummy, color };
}

function updateSpectrumBars(t, bars, bands) {
  const fade = ringWindow(t, bars.window);
  bars.group.visible = fade > 0.003;
  if (fade <= 0.003) return;
  const { mesh, dummy, color, r0, maxLen } = bars;
  for (let b = 0; b < 64; b++) {
    const v = bands[b];
    const ang = -Math.PI / 2 + (b / 64) * Math.PI * 2 + t * 0.05;
    dummy.position.set(Math.cos(ang) * r0, Math.sin(ang) * r0, 0);
    dummy.rotation.set(0, 0, ang);
    dummy.scale.set(0.22 + v * maxLen, 0.11, 0.11);
    dummy.updateMatrix();
    mesh.setMatrixAt(b, dummy.matrix);
    color.setHSL(((b / 64) + t * 0.02) % 1, 0.85, 0.22 + 0.5 * v);
    mesh.setColorAt(b, color);
  }
  mesh.instanceMatrix.needsUpdate = true;
  if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
  mesh.material.opacity = 0.9 * fade;
}

/**
 * Cue sparks: a small burst of dust per text block, tinted with the block's
 * own colour (the lyric palette rides through here), released the moment it
 * lands and gone again in about a second. The whole life cycle lives in the
 * shader from `uT` and each point's stamp, so no state is carried between
 * frames and seeking is exact.
 */
function makeCueSparks(texts) {
  const PER = 14;
  const blocks = [];
  for (let i = 0; i < texts.length; i++) {
    const tx = texts[i];
    const fade = tx.animations && tx.animations.find((a) => a.type === 'fade');
    if (!fade || !Number.isFinite(fade.start_at)) continue;
    const pos = tx.position || [0, 0, 0];
    const em = Number(tx.total_width_em) || 0;
    const size = Number(tx.size) || 1;
    blocks.push({
      cue: fade.start_at,
      pos,
      hw: Math.min(9, Math.max(1, (em * size) / 2)),
      seed: i * 131 + 7,
      col: new THREE.Color((tx.material && tx.material.color) || '#ffffff'),
    });
  }
  const n = blocks.length * PER;
  const pos = new Float32Array(n * 3);
  const aCue = new Float32Array(n);
  const aColor = new Float32Array(n * 3);
  const aSeed = new Float32Array(n * 3);
  let w = 0;
  for (const b of blocks) {
    const r = mulberry32(b.seed >>> 0);
    for (let k = 0; k < PER; k++) {
      pos[w * 3 + 0] = b.pos[0] + (r() * 2 - 1) * b.hw * 1.05;
      pos[w * 3 + 1] = b.pos[1] + (r() * 2 - 1) * 0.85;
      pos[w * 3 + 2] = b.pos[2] + (r() - 0.5) * 0.6;
      aCue[w] = b.cue + r() * 0.15;
      aColor[w * 3 + 0] = b.col.r;
      aColor[w * 3 + 1] = b.col.g;
      aColor[w * 3 + 2] = b.col.b;
      aSeed[w * 3 + 0] = Math.PI * (0.15 + 0.7 * r()); // drift mostly upward
      aSeed[w * 3 + 1] = 0.5 + r() * 1.6;
      aSeed[w * 3 + 2] = 2.0 + r() * 4.0;
      w++;
    }
  }

  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
  geo.setAttribute('aCue', new THREE.BufferAttribute(aCue, 1));
  geo.setAttribute('aColor', new THREE.BufferAttribute(aColor, 3));
  geo.setAttribute('aSeed', new THREE.BufferAttribute(aSeed, 3));
  geo.boundingSphere = new THREE.Sphere(new THREE.Vector3(0, 20, 0), 160);

  const uniforms = { uT: { value: 0 }, uOpacity: { value: 0.9 } };
  const mat = new THREE.ShaderMaterial({
    uniforms,
    transparent: true,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
    vertexShader: /* glsl */ `
      attribute float aCue;
      attribute vec3 aColor;
      attribute vec3 aSeed;
      uniform float uT;
      varying vec3 vColor;
      varying float vA;
      void main() {
        float u = (uT - aCue) / 1.15;
        float alive = step(0.0, u) * (1.0 - step(1.0, u));
        float e = 1.0 - u;
        e = e * e;
        vec3 p = position + vec3(cos(aSeed.x), sin(aSeed.x), 0.0) * (aSeed.y * u * 1.5);
        p.y += u * 0.35;
        vec4 mv = modelViewMatrix * vec4(p, 1.0);
        float size = aSeed.z * (0.45 + 0.55 * e);
        gl_PointSize = alive > 0.0 ? min(size * (240.0 / max(-mv.z, 0.001)), 26.0) : 0.0;
        vA = alive * e * e;
        vColor = aColor;
      }
    `,
    fragmentShader: /* glsl */ `
      uniform float uOpacity;
      varying vec3 vColor;
      varying float vA;
      void main() {
        if (vA <= 0.004) discard;
        float d = length(gl_PointCoord - vec2(0.5));
        float a = smoothstep(0.5, 0.0, d);
        a = a * a * vA * uOpacity;
        if (a <= 0.001) discard;
        gl_FragColor = vec4(vColor * a * 2.0, a);
      }
    `,
  });

  const points = new THREE.Points(geo, mat);
  points.frustumCulled = false;
  state.scene.add(points);
  return { points, uniforms };
}

/**
 * Roll streaks: a quiet fan of tangential speed lines around the camera's
 * look-at, invisible until the camera actually banks (a whip transition),
 * then swinging with the roll. This is the drawn element that makes a 90°
 * transition read as motion rather than as a tilt.
 */
function makeStreaks(meta) {
  const r = mulberry32(((meta.seed >>> 0) ^ 0x9e3779b9) >>> 0);
  const N = 26;
  const pos = new Float32Array(N * 4 * 3);
  const aT = new Float32Array(N * 4);
  const aC = new Float32Array(N * 4);
  const index = [];
  for (let i = 0; i < N; i++) {
    const th = (i / N) * Math.PI * 2 + (r() - 0.5) * 0.35;
    const rad = 3.6 + r() * 5.2;
    const len = 1.6 + r() * 2.8;
    const wid = 0.022 + r() * 0.035;
    const cx = Math.cos(th) * rad;
    const cy = Math.sin(th) * rad;
    const tx = -Math.sin(th);
    const ty = Math.cos(th);
    const nx = Math.cos(th);
    const ny = Math.sin(th);
    const cornersOf = (sx, sy) => [
      cx + tx * len * 0.5 * sx + nx * wid * 0.5 * sy,
      cy + ty * len * 0.5 * sx + ny * wid * 0.5 * sy,
    ];
    const quad = [cornersOf(-1, -1), cornersOf(1, -1), cornersOf(1, 1), cornersOf(-1, 1)];
    const tvals = [-1, 1, 1, -1];
    const base = i * 4;
    for (let k = 0; k < 4; k++) {
      pos.set(quad[k], (base + k) * 3);
      aT[base + k] = tvals[k];
      aC[base + k] = (rad - 3.6) / 5.2;
    }
    index.push(base, base + 1, base + 2, base, base + 2, base + 3);
  }

  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
  geo.setAttribute('aT', new THREE.BufferAttribute(aT, 1));
  geo.setAttribute('aC', new THREE.BufferAttribute(aC, 1));
  geo.setIndex(index);
  geo.boundingSphere = new THREE.Sphere(new THREE.Vector3(), 12);

  const uniforms = { uOpacity: { value: 0 } };
  const mat = new THREE.ShaderMaterial({
    uniforms,
    transparent: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    blending: THREE.AdditiveBlending,
    vertexShader: /* glsl */ `
      attribute float aT;
      attribute float aC;
      varying float vT;
      varying float vC;
      void main() {
        vT = aT;
        vC = aC;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
      }
    `,
    fragmentShader: /* glsl */ `
      uniform float uOpacity;
      varying float vT;
      varying float vC;
      void main() {
        float a = 1.0 - abs(vT);
        a = a * a * uOpacity * (0.3 + 0.5 * vC);
        if (a <= 0.003) discard;
        vec3 col = mix(vec3(0.82, 0.93, 1.0), vec3(0.45, 0.75, 1.0), vC);
        gl_FragColor = vec4(col * a * 1.6, a);
      }
    `,
  });

  const mesh = new THREE.Mesh(geo, mat);
  mesh.frustumCulled = false;
  mesh.visible = false;
  const group = new THREE.Group();
  group.add(mesh);
  state.scene.add(group);
  return { mesh, group, uniforms };
}

/**
 * Assemble every audio-driven piece. One fetch of the envelope feeds them
 * all; each carries its own visibility window, so the intro ring lives with
 * the title card, the outro ring + bars with the closing card, and the
 * sparks and streaks ride the whole song.
 */
async function buildAudioViz(meta, texts) {
  const res = await fetch('/audio_env.json');
  if (!res.ok) throw new Error(`cannot load audio_env.json: ${res.status}`);
  const env = await res.json();
  state.env = env;

  const dur = meta.total_frames / meta.fps;

  // The outro window opens on the closing card's cue.
  let outroStart = Infinity;
  for (const tx of texts) {
    const a = tx.animations && tx.animations[0];
    if (a && a.start_at > 60 && a.start_at < outroStart) outroStart = a.start_at;
  }
  if (!Number.isFinite(outroStart)) outroStart = 0;
  const outroWindow = { start: outroStart, fadeIn: 3, end: dur, fadeOut: 2 };

  // The intro window closes just before the first lyric, handing the frame
  // over together with the title card.
  const cues = texts
    .map((tx) => tx.animations && tx.animations[0] && tx.animations[0].start_at)
    .filter((v) => Number.isFinite(v))
    .sort((a, b) => a - b);
  const firstLyric = cues.find((c) => c > 8) ?? Math.min(dur * 0.1, 16.5);
  const introWindow = {
    start: 2.0,
    fadeIn: 1.6,
    end: Math.max(6, firstLyric - 0.7),
    fadeOut: 1.3,
  };

  const viz = { rings: [], bars: null, sparks: null, streaks: null, bands: new Float32Array(64) };

  if (!NORINGS) {
    viz.rings.push(
      makeSpectrumRing({
        center: [0, 0.8, -1.2],
        r0: 4.0,
        r1: 5.6,
        window: outroWindow,
      }),
    );
    // The opening ring: smaller, behind the title card and leaning back a
    // touch, so it reads as a halo rather than a dial.
    viz.rings.push(
      makeSpectrumRing({
        center: [0, 0.55, -1.9],
        r0: 2.9,
        r1: 4.3,
        tilt: -0.15,
        window: introWindow,
        hueSpeed: 0.03,
        spin: 0.05,
        p0: 0.7,
        p1: 1.7,
        opacity: 0.8,
      }),
    );
  }
  if (!NOBARS) {
    viz.bars = makeSpectrumBars({
      center: [0, 0.8, -1.4],
      r0: 6.05,
      tilt: -0.1,
      maxLen: 2.6,
      window: outroWindow,
    });
  }
  if (!NOSPARKS) viz.sparks = makeCueSparks(texts);
  if (!NOSTREAKS) viz.streaks = makeStreaks(meta);
  state.viz = viz;
}

/**
 * Timeline-driven sky. A real sphere (not a screen quad) so it stays a normal
 * piece of geometry: it gets correct depth, blurs under DOF, and blooms along
 * with everything else. Palette / nebula / star values are pushed in every
 * frame from `evalBackgrounds`, which is the only place the crossfade lives.
 */
function buildBackground(meta) {
  const keys = meta.backgrounds || [];
  state.bgKeys = keys;
  if (keys.length === 0) return;

  const uniforms = {
    uTime: { value: 0 },
    uTop: { value: new THREE.Color() },
    uBottom: { value: new THREE.Color() },
    uAccent: { value: new THREE.Color() },
    uNebula: { value: 0.5 },
    uStars: { value: 0.6 },
    uDrift: { value: 1 },
  };

  const mat = new THREE.ShaderMaterial({
    uniforms,
    side: THREE.BackSide,
    depthWrite: false,
    fog: false,
    vertexShader: /* glsl */ `
      varying vec3 vDir;
      void main() {
        vec4 wp = modelMatrix * vec4(position, 1.0);
        vDir = wp.xyz - cameraPosition;
        gl_Position = projectionMatrix * viewMatrix * wp;
      }
    `,
    fragmentShader: /* glsl */ `
      uniform float uTime, uNebula, uStars, uDrift;
      uniform vec3 uTop, uBottom, uAccent;
      varying vec3 vDir;

      float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }
      float vnoise(vec2 p) {
        vec2 i = floor(p), f = fract(p);
        vec2 u = f * f * (3.0 - 2.0 * f);
        return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
                   mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
      }
      float fbm(vec2 p) {
        float v = 0.0, a = 0.5;
        for (int i = 0; i < 2; i++) { v += a * vnoise(p); p = p * 2.03 + vec2(1.7, 9.2); a *= 0.5; }
        return v;
      }

      void main() {
        vec3 d = normalize(vDir);
        float h = clamp(d.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 col = mix(uBottom, uTop, pow(h, 0.85));

        vec2 uv = vec2(atan(d.z, d.x) / 6.2831853 + 0.5, d.y * 0.5 + 0.5);
        vec2 q = uv * vec2(7.0, 3.5) + vec2(uTime * uDrift * 0.018, uTime * uDrift * 0.009);

        float n = fbm(q * 2.0);
        float cloud = smoothstep(0.40, 0.92, n);
        // A band of accent right on the horizon reads as distance.
        float band = exp(-abs(d.y) * 5.0);
        col += uAccent * cloud * uNebula * 0.95;
        col += uAccent * band * uNebula * 0.30;

        if (uStars > 0.001) {
          vec2 sp = uv * vec2(380.0, 190.0);
          vec2 cell = floor(sp);
          float r = hash(cell);
          vec2 fp = fract(sp) - 0.5;
          vec2 jitter = vec2(hash(cell + 7.1), hash(cell + 13.7)) - 0.5;
          float star = smoothstep(0.16, 0.0, length(fp - jitter * 0.6));
          float tw = 0.55 + 0.45 * sin(uTime * 1.7 + r * 40.0);
          col += vec3(0.78, 0.86, 1.0) * star * step(r, 0.055) * tw * uStars * 1.7;
        }

        // Sink the corners a little so the frame reads as a lit stage.
        float rad = clamp(length(vec2(d.x, d.y)) * 1.15, 0.0, 1.0);
        col *= 1.0 - 0.30 * rad * rad;
        gl_FragColor = vec4(col, 1.0);
      }
    `,
  });

  const dome = new THREE.Mesh(new THREE.SphereGeometry(300, 48, 32), mat);
  dome.frustumCulled = false;
  dome.renderOrder = -1000;
  state.scene.add(dome);
  state.bgUniforms = uniforms;
}

/**
 * Grade + distortion rack, built last so it grades the finished picture.
 * Returns the pass (or null when the rack is off); uniforms live on
 * `pass.material.uniforms` because ShaderPass clones the literal it is handed.
 */
function makeFxPass(meta, w, h) {

  const fx = meta.fx;
  state.fx = fx && fx.enabled ? fx : null;
  if (!state.fx) return null;

  return new ShaderPass({
    uniforms: {
      tDiffuse: { value: null },
      uChromatic: { value: fx.chromatic || 0 },
      uVignette: { value: fx.vignette || 0 },
      uSaturation: { value: fx.saturation ?? 1 },
      uFlash: { value: 0 },
      uHit: { value: 0 },
      uTime: { value: 0 },
    },
    vertexShader: /* glsl */ `
      varying vec2 vUv;
      void main() {
        vUv = uv;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
      }
    `,
    fragmentShader: /* glsl */ `
      uniform sampler2D tDiffuse;
      uniform float uChromatic, uVignette, uSaturation, uFlash, uHit, uTime;
      varying vec2 vUv;

      void main() {
        vec2 c = vUv - 0.5;
        float r2 = dot(c, c);

        // Chromatic aberration grows toward the frame edge and spikes on a cue
        // hit, which is what sells a line landing.
        float k = uChromatic * (1.0 + 7.0 * uHit) * (0.35 + r2 * 2.4);
        vec2 off = c * k;
        vec3 col = vec3(
          texture2D(tDiffuse, vUv + off).r,
          texture2D(tDiffuse, vUv).g,
          texture2D(tDiffuse, vUv - off).b
        );

        float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = mix(vec3(l), col, uSaturation);

        float rad = clamp(length(c) * 1.45, 0.0, 1.0);
        col *= 1.0 - uVignette * rad * rad;


        col = mix(col, vec3(1.0), clamp(uFlash, 0.0, 1.0));
        gl_FragColor = vec4(max(col, 0.0), 1.0);
      }
    `,
  });
}

function buildComposer(meta, w, h) {
  const rt = new THREE.WebGLRenderTarget(w, h, {
    type: THREE.HalfFloatType,
    samples: 4,
  });
  const composer = new EffectComposer(state.renderer, rt);
  composer.setSize(w, h);

  composer.addPass(new RenderPass(state.scene, state.camera));

  if (meta.bloom && meta.bloom.enabled) {
    const radius = Math.min(1, Math.max(0, meta.bloom.scale / 4));
    composer.addPass(
      new UnrealBloomPass(
        new THREE.Vector2(w, h),
        meta.bloom.intensity,
        radius,
        meta.bloom.threshold,
      ),
    );
  }

  if (meta.dof && meta.dof.enabled) {
    const focus = meta.dof.focal_distance;
    // Artistic, not physical: at this scene scale a literal f-number works out
    // to sub-pixel blur, so the f-stop only controls how fast blur grows with
    // depth (lower number = shallower field). Calibrated so the focal plane
    // stays sharp while foreground/background particles soften into bokeh.
    const aperture = 0.00017 * (4.0 / Math.max(meta.dof.aperture_f_stops, 0.1));
    const maxblur = 0.008 * Math.max(meta.dof.focal_radius, 0.001);
    composer.addPass(
      new BokehPass(state.scene, state.camera, { focus, aperture, maxblur }),
    );
  }

  composer.addPass(new OutputPass());

  if (meta.fxaa) {
    const pass = new ShaderPass(FXAAShader);
    pass.material.uniforms.resolution.value.set(1 / w, 1 / h);
    composer.addPass(pass);
  }

  // Grade last: vignette / chromatic / flash belong on the finished antialiased
  // picture, not on the linear buffer bloom and bokeh were fed.
  state.fxPass = makeFxPass(meta, w, h);
  if (state.fxPass) composer.addPass(state.fxPass);

  state.composer = composer;
}

function setupCamera(meta, w, h) {
  const cam = new THREE.PerspectiveCamera(
    (meta.camera.fov * 180) / Math.PI,
    w / h,
    meta.camera.near,
    meta.camera.far,
  );
  cam.position.set(0, 1, 26);
  cam.lookAt(0, 1, 0);
  state.camera = cam;

  if (FRONT) return;

  const ca = meta.camera_animation;
  if (ca && ca.type === 'orbit') {
    state.orbit = ca;
  }
}

function updateCamera(t) {
  const o = state.orbit;
  const fx = state.fx;
  // The rack is optional; with no cues or no rack the punch stays dead zero.
  const hit = fx ? hitEnvelope(state.cues, t, fx.hit_duration ?? 0.35) : 0;
  state.lastHit = hit;
  if (!o) return;

  // Orbit, the eased push/crane/pan and any per-cue stops all resolve through
  // one shared pure rig (`evalOrbit`), so the off-frame maths that decides when
  // a line has left the picture works from exactly the framing drawn here.
  const rig = evalOrbit(o, t);
  const yaw = rig.yaw;
  // The cloud is authored around the world origin, which is where the default
  // rig looks. When stops walk the camera across a lyric field the cloud has
  // to come along or a wide shot would find bare sky — but only part of the
  // way, so the drift between particles and camera still reads as depth. The
  // rig target is used before the shake displaces it, because the shake moves
  // eye and look-at together and the world already swings on screen for free.
  // With no stops this is exactly zero and the cloud sits exactly where it
  // always has.
  if (state.particles) {
    const base = o.target || [0, 0, 0];
    state.followOffset = [
      (rig.target[0] - base[0]) * 0.75,
      (rig.target[1] - base[1]) * 0.75,
      (rig.target[2] - base[2]) * 0.75,
    ];
    state.particles.position.set(
      state.followOffset[0],
      state.followOffset[1],
      state.followOffset[2],
    );
  } else {
    const base = o.target || [0, 0, 0];
    state.followOffset = [
      (rig.target[0] - base[0]) * 0.75,
      (rig.target[1] - base[1]) * 0.75,
      (rig.target[2] - base[2]) * 0.75,
    ];
  }
  let tx = rig.target[0];
  let ty = rig.target[1];
  let tz = rig.target[2];
  const ce = Math.cos(rig.elevation);
  const se = Math.sin(rig.elevation);
  let px = tx + rig.radius * ce * Math.cos(yaw);
  let py = ty + rig.radius * se;
  let pz = tz + rig.radius * ce * Math.sin(yaw);

  let roll = rig.roll || 0;
  const s = o.shake;
  // `shakeWeight` gates on the cues directly, so a cue-gated rig still kicks
  // when the FX rack is off.
  const env = s ? shakeWeight(s, t, state.cues) : 0;
  if (env > 0) {
    const amp = (s.amplitude ?? 0) * env;
    const n = jitterNoise(t, 2 * Math.PI * Math.max(s.frequency ?? 3.5, 0), 0.7);
    // Displace the whole rig along its own right/up axes: the picture then
    // wanders on screen whichever way the orbit happens to be facing.
    let dx = px - tx;
    let dy = py - ty;
    let dz = pz - tz;
    const dl = Math.hypot(dx, dy, dz) || 1;
    dx /= dl;
    dy /= dl;
    dz /= dl;
    // right = normalize(cross(up, dir)) with world up (0, 1, 0)
    let rx = dz;
    let ry = 0;
    let rz = -dx;
    const rl = Math.hypot(rx, ry, rz) || 1;
    rx /= rl;
    ry /= rl;
    rz /= rl;
    // up' = cross(dir, right)
    const ux = dy * rz - dz * ry;
    const uy = dz * rx - dx * rz;
    const uz = dx * ry - dy * rx;
    const ox = (rx * n[0] + ux * n[1] + dx * n[2] * 0.5) * amp;
    const oy = (ry * n[0] + uy * n[1] + dy * n[2] * 0.5) * amp;
    const oz = (rz * n[0] + uz * n[1] + dz * n[2] * 0.5) * amp;
    px += ox;
    py += oy;
    pz += oz;
    tx += ox;
    ty += oy;
    tz += oz;
    roll = (rig.roll || 0) + n[1] * (s.roll ?? 0) * env;
  }

  state.camTarget = [tx, ty, tz];

  // Cue punch: slide the camera toward the look-at point so every line lands
  // with a little forward weight. Displacing only the eye keeps the framing,
  // which is what a "push" reads as rather than a zoom. The push spends
  // itself with the music: a quiet passage gets a touch, a loud one the step.
  if (hit > 0 && fx.punch) {
    const dx = tx - px;
    const dy = ty - py;
    const dz = tz - pz;
    const dl = Math.hypot(dx, dy, dz) || 1;
    const step = fx.punch * hit * (0.55 + 0.45 * state.energyNow);
    px += (dx / dl) * step;
    py += (dy / dl) * step;
    pz += (dz / dl) * step;
  }

  state.camera.position.set(px, py, pz);
  state.camera.lookAt(tx, ty, tz);
  if (roll !== 0) state.camera.rotateZ(roll);
  // The bank the picture is carrying this instant, stops + shake together —
  // landing glyphs follow it in and settle level as it unwinds.
  state.camRoll = roll;
}

// ---------------------------------------------------------------------------
// per-frame update
// ---------------------------------------------------------------------------

/** Push the timeline-driven backdrop and the grade rack for this instant. */
function applyBackdrop(t) {
  if (state.bgUniforms) {
    const bg = evalBackgrounds(state.bgKeys, t);
    if (bg) {
      const u = state.bgUniforms;
      u.uTime.value = t;
      // Authored as sRGB; `setRGB` with an explicit space converts to the
      // linear working space exactly once, matching how THREE.Color reads hex.
      u.uTop.value.setRGB(bg.top[0], bg.top[1], bg.top[2], THREE.SRGBColorSpace);
      u.uBottom.value.setRGB(bg.bottom[0], bg.bottom[1], bg.bottom[2], THREE.SRGBColorSpace);
      u.uAccent.value.setRGB(bg.accent[0], bg.accent[1], bg.accent[2], THREE.SRGBColorSpace);
      u.uNebula.value = bg.nebula;
      u.uStars.value = bg.stars;
      u.uDrift.value = bg.drift;
    }
  }
  if (state.fxPass) {
    const u = state.fxPass.material.uniforms;
    u.uHit.value = state.lastHit;
    // Like the camera punch, the flash rides the music's energy instead of
    // firing at full strength through a whisper-quiet cue.
    u.uFlash.value =
      (state.fx.flash || 0) * state.lastHit * (0.45 + 0.55 * state.energyNow);
    u.uTime.value = t;
  }
}

/** Read the envelope for this instant: bands for every visualiser, the bass
 *  for the dust cloud and the mean energy the punch / flash spend. Runs
 *  before the camera moves so a seek is exact, never a frame stale. */
function updateEnvFrame(t) {
  const env = state.env;
  const viz = state.viz;
  const fi = Math.min(env.frames - 1, Math.max(0, Math.round(t * env.fps)));
  const row = env.data[fi] || [];
  const bands = viz.bands;
  let bass = 0;
  let energy = 0;
  for (let i = 0; i < 64; i++) {
    const v = row.length
      ? row[Math.min(row.length - 1, Math.floor((i / 64) * row.length))]
      : 0;
    bands[i] = v;
    if (i < 8) bass += v;
    energy += v;
  }
  bass /= 8;
  energy /= 64;
  state.energyNow = energy;
  for (const r of viz.rings) r.uniforms.uBand.value.set(bands);

  // The dust cloud breathes with the bass wherever a ring is on screen;
  // outside both windows this is exactly its constructed 0.9.
  let breath = 0;
  for (const r of viz.rings) breath = Math.max(breath, ringWindow(t, r.window));
  if (state.particleUniforms) {
    state.particleUniforms.uOpacity.value = 0.9 * (1 + 0.28 * bass * breath);
  }
}

function updateViz(t) {
  const v = state.viz;
  for (const r of v.rings) {
    const fade = ringWindow(t, r.window);
    r.uniforms.uFade.value = fade;
    r.uniforms.uTime.value = t;
    r.points.visible = fade > 0.003;
  }
  if (v.bars) updateSpectrumBars(t, v.bars, v.bands);
  if (v.sparks) v.sparks.uniforms.uT.value = t;
  if (v.streaks) {
    // The streaks only exist while the camera is actually banked, and swing
    // with the roll, which is what makes a whip read as speed lines.
    const roll = state.camRoll;
    const op = Math.min(1, Math.max(0, (Math.abs(roll) - 0.22) / 0.9)) * 0.85;
    v.streaks.mesh.visible = op > 0.01;
    if (op > 0.01) {
      const fo = state.followOffset || [0, 0, 0];
      v.streaks.group.position.set(fo[0], fo[1], fo[2] - 0.7);
      v.streaks.mesh.rotation.z = roll * 0.92;
      v.streaks.uniforms.uOpacity.value = op;
    }
  }
}

function setFrame(t) {
  if (state.env && state.viz) updateEnvFrame(t);
  updateCamera(t);
  applyBackdrop(t);

  const tmpQ = new THREE.Quaternion();
  const followRoll = state.camRoll;
  for (const c of state.chars) {
    const st = evalAll(c.animations, t, c.idx, c.size, c.charTimes);
    c.mesh.position.set(
      c.basePosition.x + st.offset[0],
      c.basePosition.y + st.offset[1],
      c.basePosition.z + st.offset[2],
    );
    tmpQ.set(st.rot[0], st.rot[1], st.rot[2], st.rot[3]);
    c.mesh.quaternion.copy(c.baseRotation).multiply(tmpQ);
    // A line landing under a banked camera enters along the bank: for the
    // first beat of its life each glyph carries the camera's roll and then
    // settles level as the camera does. That is what ties a whip transition
    // to the word it delivers — the text arrives *with* the rotation.
    if (followRoll !== 0 && c.landsAt !== null && st.alpha > 0.003) {
      const u = t - c.landsAt;
      if (u > -0.02 && u < 1.05) {
        const w = u <= 0.45 ? 1 : 1 - smoothstep((u - 0.45) / 0.6);
        if (w > 0.001) c.mesh.rotateZ(followRoll * w);
      }
    }
    c.mesh.scale.set(
      c.baseScale.x * st.scale,
      c.baseScale.y * st.scale,
      c.baseScale.z * st.scale,
    );
    const op = c.baseOpacity * c.textAlpha * st.alpha;
    c.material.opacity = op;
    if (c.outlineMaterial) c.outlineMaterial.uniforms.uOpacity.value = op;
    // A lyric scene carries every line for the whole song; skipping the ones
    // at zero opacity keeps the draw-call count proportional to what's lit.
    // A char popped to zero scale is skipped for the same reason.
    c.mesh.visible = op > 0.002 && st.scale > 1e-3;
  }

  if (state.particleUniforms) state.particleUniforms.uTime.value = t;

  if (state.viz) updateViz(t);

  if (NOPOST) state.renderer.render(state.scene, state.camera);
  else state.composer.render();
}

// ---------------------------------------------------------------------------
// boot
// ---------------------------------------------------------------------------

async function main() {
  const res = await fetch('/scene_data.json');
  if (!res.ok) throw new Error(`cannot load scene_data.json: ${res.status}`);
  const data = await res.json();
  const meta = data.meta;
  state.meta = meta;

  const w = meta.width;
  const h = meta.height;
  state.width = w;
  state.height = h;

  const renderer = new THREE.WebGLRenderer({
    antialias: false,
    alpha: false,
    preserveDrawingBuffer: true,
  });
  renderer.setPixelRatio(1);
  renderer.setSize(w, h);
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1.0;
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.setClearColor(new THREE.Color(meta.background), 1);
  document.body.appendChild(renderer.domElement);
  state.renderer = renderer;

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(DEBUG ? '#c80000' : meta.background);
  state.scene = scene;

  // Every moment a line lights up. One list drives the flash, the chromatic
  // spike and the camera punch, so sync comes from the timeline for free.
  state.cues = data.texts
    .map((tx) => (tx.animations.find((a) => a.type === 'fade') || {}).start_at)
    .filter((v) => Number.isFinite(v))
    .sort((a, b) => a - b);

  setupCamera(meta, w, h);
  if (!NOLIGHTS) buildLights(meta.lights);
  buildBackground(meta);
  if (BOX) {
    const box = new THREE.Mesh(
      new THREE.BoxGeometry(3, 3, 3),
      new THREE.MeshBasicMaterial({ color: 0x00ff00 }),
    );
    box.position.set(0, 1, 0);
    state.scene.add(box);
  }
  for (let i = 0; i < data.texts.length; i++) {
    if (DEBUG_TEXT !== null && Number(DEBUG_TEXT) !== i) continue;
    buildText(data.texts[i], i);
  }
  buildParticles(meta);
  if (AUDIO_ENV) await buildAudioViz(meta, data.texts);
  buildComposer(meta, w, h);

  updateCamera(0);
  setFrame(0);
  await new Promise((r) => requestAnimationFrame(r));

  window.__SCENE_INFO__ = {
    width: w,
    height: h,
    totalFrames: meta.total_frames,
    fps: meta.fps,
    chars: state.chars.length,
    camera: state.camera.position.toArray(),
    debug: DEBUG,
    search: typeof location !== 'undefined' ? location.search : '',
    canvases: document.querySelectorAll('canvas').length,
    points: state.scene.children.filter((o) => o.isPoints).length,
    sample: state.chars.slice(0, 3).map((c) => ({
      pos: c.mesh.position.toArray().map((n) => +n.toFixed(3)),
      scale: c.mesh.scale.toArray(),
      op: +c.material.opacity.toFixed(3),
      vis: c.mesh.visible,
      tr: c.material.transparent,
      r: c.mesh.geometry.boundingSphere
        ? +c.mesh.geometry.boundingSphere.radius.toFixed(3)
        : null,
      err: (() => {
        const e = c.mesh.geometry;
        return e.attributes.position.array.some((n) => !Number.isFinite(n)) ? 'NaN' : 'ok';
      })(),
    })),
  };
  window.setFrame = async (t) => {
    setFrame(t);
    window.__PROBE__ = {
      t,
      camera: state.camera.position.toArray().map((n) => +n.toFixed(3)),
      chars: state.chars.slice(0, 4).map((c) => ({
        p: c.mesh.position.toArray().map((n) => +n.toFixed(3)),
        s: c.mesh.scale.toArray(),
        q: c.mesh.quaternion.toArray().map((n) => +n.toFixed(3)),
        o: +c.material.opacity.toFixed(3),
        v: c.mesh.visible,
        inScene: !!c.mesh.parent,
      })),
      calls: state.renderer.info.render.calls,
      tris: state.renderer.info.render.triangles,
    };
    // A lyric scene carries every line for the whole song; how many are lit
    // right now is the one number that tells you the timeline is working.
    const lit = state.chars.filter((c) => c.mesh.visible);
    window.__PROBE__.visible = lit.length;
    window.__PROBE__.litSample = lit.slice(0, 3).map((c) => ({
      p: c.mesh.position.toArray().map((n) => +n.toFixed(2)),
      o: +c.material.opacity.toFixed(2),
    }));
    await new Promise((r) => requestAnimationFrame(r));
    const gl = state.renderer.getContext();
    const px = (x, y) => {
      const b = new Uint8Array(4);
      gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
      return Array.from(b);
    };
    window.__PROBE__.gl = {
      center: px(960, 540),
      boxEdge: px(960 + 150, 540),
      glyph1: px(735, 1080 - 427),
      canvasW: state.renderer.domElement.width,
      canvasH: state.renderer.domElement.height,
      rect: (() => {
        const r = state.renderer.domElement.getBoundingClientRect();
        return [r.x, r.y, r.width, r.height];
      })(),
      rt: state.renderer.getRenderTarget() ? 'FBO' : 'canvas',
    };
  };
  window.__READY__ = true;
}

main().catch((err) => {
  window.__ERROR__ = String((err && err.stack) || err);
  window.__READY__ = true;
  console.error(err);
});
