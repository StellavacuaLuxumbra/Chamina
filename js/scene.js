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

import { clamp01, easeApply, evalAll, evalBackgrounds, hitEnvelope, jitterNoise } from './anim.mjs';

// Debug flags (set through `render.js --url "a=1&b=1"`):
//   debug      all of the below at once (head-on camera, no post, no particles)
//   front      fixed head-on camera (ignore camera_animation)
//   nopost     render straight to the canvas (skip EffectComposer)
//   noparticles, nolights, text=<n>   (text=<n> keeps only that line)
//   nooutline    skip the inverted-hull rim (A/B its look)
//   box        add a 3x3x3 green reference cube at the origin (off by default)
const QS = new URLSearchParams(typeof location !== 'undefined' ? location.search : '');
const flag = (k) => QS.has(k);
const DEBUG = flag('debug');
const FRONT = DEBUG || flag('front');
const NOPOST = DEBUG || flag('nopost');
const NOPARTICLES = DEBUG || flag('noparticles');
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

/**
 * Mirror of Rust's `CameraShake::envelope_at`: fades in over `ramp`, holds,
 * fades out over `ramp` before `until` (and never below zero).
 */
function shakeEnvelope(s, t) {
  const at = s.at ?? 0;
  if (t < at) return 0;
  const until = s.until;
  const ramp = Math.max(s.ramp ?? 0.6, 1e-6);
  if (until !== null && until !== undefined && t > until) return 0;
  const out = until === null || until === undefined ? 1 : clamp01((until - t) / ramp);
  return Math.min(clamp01((t - at) / ramp), out);
}

function updateCamera(t) {
  const o = state.orbit;
  const fx = state.fx;
  // The rack is optional; with no cues or no rack the punch stays dead zero.
  const hit = fx ? hitEnvelope(state.cues, t, fx.hit_duration ?? 0.35) : 0;
  state.lastHit = hit;
  if (!o) return;

  let radius = o.radius;
  let elevation = o.elevation;
  let tx = o.target[0];
  let ty = o.target[1];
  let tz = o.target[2];

  // Eased push / crane / pan layered on top of the orbit.
  const m = o.movement;
  if (m) {
    const k = easeApply(
      m.ease ?? 'ease_in_out_cubic',
      (t - (m.at ?? 0)) / Math.max(m.duration ?? 1, 1e-6),
    );
    if (m.radius_end !== null && m.radius_end !== undefined) {
      radius += (m.radius_end - radius) * k;
    }
    if (m.elevation_end !== null && m.elevation_end !== undefined) {
      elevation += (m.elevation_end - elevation) * k;
    }
    if (m.target_end) {
      tx += (m.target_end[0] - tx) * k;
      ty += (m.target_end[1] - ty) * k;
      tz += (m.target_end[2] - tz) * k;
    }
  }

  const yaw = o.start_yaw + o.speed * t;
  const ce = Math.cos(elevation);
  const se = Math.sin(elevation);
  let px = tx + radius * ce * Math.cos(yaw);
  let py = ty + radius * se;
  let pz = tz + radius * ce * Math.sin(yaw);

  let roll = 0;
  const s = o.shake;
  const env = s ? shakeEnvelope(s, t) : 0;
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
    roll = n[1] * (s.roll ?? 0) * env;
  }

  state.camTarget = [tx, ty, tz];

  // Cue punch: slide the camera toward the look-at point so every line lands
  // with a little forward weight. Displacing only the eye keeps the framing,
  // which is what a "push" reads as rather than a zoom.
  if (hit > 0 && fx.punch) {
    const dx = tx - px;
    const dy = ty - py;
    const dz = tz - pz;
    const dl = Math.hypot(dx, dy, dz) || 1;
    const step = fx.punch * hit;
    px += (dx / dl) * step;
    py += (dy / dl) * step;
    pz += (dz / dl) * step;
  }

  state.camera.position.set(px, py, pz);
  state.camera.lookAt(tx, ty, tz);
  if (roll !== 0) state.camera.rotateZ(roll);
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
    u.uFlash.value = (state.fx.flash || 0) * state.lastHit;
    u.uTime.value = t;
  }
}

function setFrame(t) {
  updateCamera(t);
  applyBackdrop(t);

  const tmpQ = new THREE.Quaternion();
  for (const c of state.chars) {
    const st = evalAll(c.animations, t, c.idx, c.size);
    c.mesh.position.set(
      c.basePosition.x + st.offset[0],
      c.basePosition.y + st.offset[1],
      c.basePosition.z + st.offset[2],
    );
    tmpQ.set(st.rot[0], st.rot[1], st.rot[2], st.rot[3]);
    c.mesh.quaternion.copy(c.baseRotation).multiply(tmpQ);
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