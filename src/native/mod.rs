//! Native renderer: the whole Three.js stage (`js/scene.js`) as plain Rust.
//!
//! Input is the same bridge document (`scene_data.json`) the browser renderer
//! eats; output is raw sRGB frames piped to ffmpeg. No browser, no WebGL, no
//! GPU driver — every pixel is computed here, deterministically, and the
//! arithmetic is ported line-for-line from `js/scene.js` / `js/anim.mjs`
//! (the animation layer itself is *reused* from `crate::animation`, which the
//! JS side already mirrors).
//!
//! Deviations from the browser, both deliberate:
//!  * the outro visualiser window is derived from the card blocks (texts
//!    without word timing) instead of any `start_at > 60` — the JS rule
//!    accidentally caught the first lyric past the minute mark;
//!  * no MSAA: FXAA at the end of the chain is the only edge smoothing,
//!    exactly like the browser chain's final pass.

pub mod draw;
pub mod pipe;
pub mod post;

use std::path::Path;

use serde::Deserialize;

use crate::animation::{self, AnimationSpec};
use crate::export::SceneData;
use crate::math::{Quat, Vec3};
use crate::scene::{BackgroundKey, FxConfig, OrbitCam};

// ---------------------------------------------------------------------------
// audio envelope (js/audio-env.mjs output)
// ---------------------------------------------------------------------------

#[derive(Debug, Deserialize)]
pub struct AudioEnv {
    pub fps: f32,
    pub bands: usize,
    pub frames: usize,
    pub data: Vec<Vec<f32>>,
}

impl AudioEnv {
    pub fn load(path: &Path) -> std::io::Result<Self> {
        let s = std::fs::read_to_string(path)?;
        Ok(serde_json::from_str(&s)?)
    }

    /// Band energies at `t`, plus the bass (first 8 bands) and mean energy
    /// the punch / flash spend. Row indexing matches
    /// `js/scene.js::updateEnvFrame` (floor-scaled band → row index).
    pub fn sample(&self, t: f32) -> ([f32; 64], f32, f32) {
        let mut bands = [0.0f32; 64];
        let fi = (t * self.fps).round() as i64;
        let fi = fi.clamp(0, self.frames as i64 - 1) as usize;
        let row = &self.data[fi];
        let n = row.len();
        let mut sum = 0.0;
        for (i, b) in bands.iter_mut().enumerate() {
            let v = row[((i * n) / 64).min(n - 1)];
            *b = v;
            sum += v;
        }
        let bass = bands[..8].iter().sum::<f32>() / 8.0;
        (bands, bass, sum / 64.0)
    }
}

// ---------------------------------------------------------------------------
// camera rig (port of js/anim.mjs::evalOrbit + js/scene.js::updateCamera)
// ---------------------------------------------------------------------------

/// Pure rig state: where the camera looks and how it is banked, before shake
/// and the cue punch.
#[derive(Debug, Clone, Copy)]
pub struct Rig {
    pub radius: f32,
    pub elevation: f32,
    pub target: Vec3,
    pub yaw: f32,
    pub roll: f32,
}

fn ease_apply(e: crate::animation::Ease, x: f32) -> f32 {
    e.apply(x)
}

/// Port of `evalOrbit`: orbit → movement → stops, stops eases from wherever
/// the previous one landed and only overrides the axes it names.
pub fn eval_orbit(o: &OrbitCam, t: f32) -> Rig {
    let mut radius = o.radius;
    let mut elevation = o.elevation;
    let mut tx = o.target.x;
    let mut ty = o.target.y;
    let mut tz = o.target.z;
    let mut yaw_off = 0.0f32;
    let mut roll_off = 0.0f32;

    if let Some(m) = &o.movement {
        let k = ease_apply(m.ease, (t - m.at) / m.duration.max(1e-6));
        if let Some(r) = m.radius_end {
            radius = radius + (r - radius) * k;
        }
        if let Some(e) = m.elevation_end {
            elevation = elevation + (e - elevation) * k;
        }
        if let Some(te) = m.target_end {
            tx += (te.x - tx) * k;
            ty += (te.y - ty) * k;
            tz += (te.z - tz) * k;
        }
    }

    for s in &o.stops {
        if t < s.at {
            break;
        }
        let k = ease_apply(s.ease, (t - s.at) / s.duration.max(1e-6));
        let tr = s.radius.unwrap_or(radius);
        let te = s.elevation.unwrap_or(elevation);
        let (ttx, tty, ttz) = match s.target {
            Some(p) => (p.x, p.y, p.z),
            None => (tx, ty, tz),
        };
        let tyaw = s.yaw.unwrap_or(yaw_off);
        let troll = s.roll.unwrap_or(roll_off);
        radius += (tr - radius) * k;
        elevation += (te - elevation) * k;
        tx += (ttx - tx) * k;
        ty += (tty - ty) * k;
        tz += (ttz - tz) * k;
        yaw_off += (tyaw - yaw_off) * k;
        roll_off += (troll - roll_off) * k;
    }

    Rig {
        radius,
        elevation,
        target: Vec3::new(tx, ty, tz),
        yaw: o.start_yaw + o.speed * t + yaw_off,
        roll: roll_off,
    }
}

/// Port of `shakeEnvelope` + `shakeWeight`: window envelope, optionally gated
/// on the lyric cues.
fn shake_weight(s: &crate::scene::CameraShake, t: f32, cues: &[f32], hit_dur: f32) -> f32 {
    let at = s.at;
    if t < at {
        return 0.0;
    }
    let ramp = s.ramp.max(1e-6);
    let out = match s.until {
        Some(u) if t > u => return 0.0,
        Some(u) => ((u - t) / ramp).clamp(0.0, 1.0),
        None => 1.0,
    };
    let env = ((t - at) / ramp).clamp(0.0, 1.0).min(out);
    if env <= 0.0 {
        return 0.0;
    }
    if s.hit {
        env * hit_envelope(cues, t, hit_dur)
    } else {
        env
    }
}

/// `(1-u)²` decay from the moment a line lands; the largest hit wins.
pub fn hit_envelope(cues: &[f32], t: f32, dur: f32) -> f32 {
    let mut best = 0.0f32;
    for &c in cues {
        let u = (t - c) / dur.max(1e-6);
        if (0.0..=1.0).contains(&u) {
            let v = (1.0 - u) * (1.0 - u);
            if v > best {
                best = v;
            }
        }
    }
    best
}

/// Resolved camera for a frame: eye position, basis and roll, with shake and
/// the cue punch already applied. Mirrors `updateCamera` including the
/// music-scaled punch.
pub struct Cam {
    pub eye: Vec3,
    pub target: Vec3,
    pub fwd: Vec3,
    pub right: Vec3,
    pub up: Vec3,
    pub roll: f32,
    pub tan_h: f32,
    pub aspect: f32,
    pub near: f32,
}

#[allow(clippy::too_many_arguments)]
pub fn update_camera(
    o: &OrbitCam,
    t: f32,
    cues: &[f32],
    fx: Option<&FxConfig>,
    energy: f32,
    fov: f32,
    aspect: f32,
    near: f32,
) -> Cam {
    let rig = eval_orbit(o, t);
    let hit = fx
        .map(|f| hit_envelope(cues, t, f.hit_duration))
        .unwrap_or(0.0);

    let mut eye = rig.target
        + Vec3::new(
            rig.radius * rig.elevation.cos() * rig.yaw.cos(),
            rig.radius * rig.elevation.sin(),
            rig.radius * rig.elevation.cos() * rig.yaw.sin(),
        );
    let mut target = rig.target;
    let mut roll = rig.roll;

    if let Some(s) = &o.shake {
        let env = shake_weight(s, t, cues, s.hit_duration);
        if env > 0.0 {
            let amp = s.amplitude * env;
            let n = animation::jitter_noise(t, std::f32::consts::TAU * s.frequency.max(0.0), 0.7);
            let dir = (eye - target).normalize();
            let right = Vec3::new(dir.z, 0.0, -dir.x).normalize();
            let up = dir.cross(right);
            let off = right * (n.x * amp) + up * (n.y * amp) + dir * (n.z * amp * 0.5);
            eye += off;
            target += off;
            roll += n.y * s.roll * env;
        }
    }

    if hit > 0.0 {
        if let Some(f) = fx {
            if f.punch > 0.0 {
                let dir = target - eye;
                let step = f.punch * hit * (0.55 + 0.45 * energy);
                eye += dir.normalize() * step;
            }
        }
    }

    let fwd = (target - eye).normalize();
    // right = normalize(cross(forward, worldUp)) — matches js cameraBasis.
    let right = Vec3::new(-fwd.z, 0.0, fwd.x).normalize();
    let up = right.cross(fwd);
    // camera.rotateZ(roll): the bank is part of the view basis itself, so
    // every projection (text, dome, streaks) sees the world tilted.
    let (cr, sr) = (roll.cos(), roll.sin());
    let right = right * cr + up * sr;
    let up = up * cr - right * sr;
    Cam {
        eye,
        target,
        fwd,
        right,
        up,
        roll,
        tan_h: (fov * 0.5).tan(),
        aspect,
        near,
    }
}

impl Cam {
    /// View-space coords `(xc, yc, zc)` of a world point; `None` behind the
    /// near plane.
    pub fn view(&self, p: Vec3) -> Option<(f32, f32, f32)> {
        let v = p - self.eye;
        let zc = v.dot(self.fwd);
        if zc <= self.near {
            return None;
        }
        Some((v.dot(self.right), v.dot(self.up), zc))
    }

    /// Normalised device coords `[-1,1]²` + view depth.
    pub fn ndc(&self, p: Vec3) -> Option<(f32, f32, f32)> {
        let (xc, yc, zc) = self.view(p)?;
        Some((
            (xc / zc) / (self.tan_h * self.aspect),
            (yc / zc) / self.tan_h,
            zc,
        ))
    }
}

// ---------------------------------------------------------------------------
// background timeline (port of js/anim.mjs::evalBackgrounds)
// ---------------------------------------------------------------------------

fn smoothstep(x: f32) -> f32 {
    let v = x.clamp(0.0, 1.0);
    v * v * (3.0 - 2.0 * v)
}

fn mix(a: [f32; 3], b: [f32; 3], k: f32) -> [f32; 3] {
    [
        a[0] + (b[0] - a[0]) * k,
        a[1] + (b[1] - a[1]) * k,
        a[2] + (b[2] - a[2]) * k,
    ]
}

/// sRGB 0..1 → linear (what `new THREE.Color('#hex')` does under color
/// management).
pub fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

pub fn rgb_linear(c: crate::color::Color) -> [f32; 3] {
    [
        srgb_to_linear(c.r),
        srgb_to_linear(c.g),
        srgb_to_linear(c.b),
    ]
}

/// The active look at `t`, crossfaded from its predecessor **in sRGB** (the
/// JS mixes before converting; matching that keeps the fades identical).
pub fn eval_backgrounds(keys: &[BackgroundKey], t: f32) -> Option<[f32; 12]> {
    let mut idx = 0;
    for (k, key) in keys.iter().enumerate() {
        if key.at <= t {
            idx = k;
        }
    }
    let cur = &keys[idx];
    let top = [cur.top.r, cur.top.g, cur.top.b];
    let bottom = [cur.bottom.r, cur.bottom.g, cur.bottom.b];
    let accent = [cur.accent.r, cur.accent.g, cur.accent.b];
    let mut out = [
        top[0], top[1], top[2], bottom[0], bottom[1], bottom[2], accent[0], accent[1], accent[2],
    ];
    let mut nebula = cur.nebula;
    let mut stars = cur.stars;
    let mut drift = cur.drift;
    if idx > 0 {
        let prev = &keys[idx - 1];
        let fade = (cur.fade).max(0.0);
        if fade > 0.0 {
            let k = smoothstep((t - cur.at) / fade);
            let p = [
                [prev.top.r, prev.top.g, prev.top.b],
                [prev.bottom.r, prev.bottom.g, prev.bottom.b],
                [prev.accent.r, prev.accent.g, prev.accent.b],
            ];
            let c = [top, bottom, accent];
            for i in 0..3 {
                let m = mix(p[i], c[i], k);
                out[i * 3] = m[0];
                out[i * 3 + 1] = m[1];
                out[i * 3 + 2] = m[2];
            }
            nebula = prev.nebula + (cur.nebula - prev.nebula) * k;
            stars = prev.stars + (cur.stars - prev.stars) * k;
            drift = prev.drift + (cur.drift - prev.drift) * k;
        }
    }
    let mut full = [0.0f32; 12];
    full[..9].copy_from_slice(&out);
    full[9] = nebula;
    full[10] = stars;
    full[11] = drift;
    Some(full)
}

// ---------------------------------------------------------------------------
// prepared scene
// ---------------------------------------------------------------------------

/// The outro visualiser's start: the first card block (a text without word
/// timing) that starts after the one-minute mark. Lyrics carry `char_times`
/// and are excluded — the JS rule keyed on `start_at > 60` alone and caught
/// the first lyric past the minute.
fn outro_start(data: &SceneData) -> f32 {
    let mut best = f32::INFINITY;
    for tx in &data.texts {
        if tx.char_times.is_some() {
            continue;
        }
        if let Some(a0) = tx.animations.first() {
            let at = anim_start(a0);
            if at > 60.0 && at < best {
                best = at;
            }
        }
    }
    if best.is_finite() {
        best
    } else {
        0.0
    }
}

fn anim_start(a: &AnimationSpec) -> f32 {
    match a {
        AnimationSpec::Fall(f) => f.start_at,
        AnimationSpec::Fade(f) => f.start_at,
        AnimationSpec::Spin(s) => s.start_delay,
        AnimationSpec::Slide(s) => s.start_at,
        AnimationSpec::Jitter(j) => j.start_at,
        AnimationSpec::Pop(p) => p.start_at,
        AnimationSpec::Wave(w) => w.start_at,
    }
}

fn cue_list(data: &SceneData) -> Vec<f32> {
    let mut cues = Vec::new();
    for tx in &data.texts {
        for a in &tx.animations {
            if let AnimationSpec::Fade(f) = a {
                cues.push(f.start_at);
            }
        }
    }
    cues.sort_by(|a, b| a.partial_cmp(b).unwrap());
    cues
}

/// One visualiser ring window (`ringWindow` in js/scene.js).
#[derive(Debug, Clone, Copy)]
pub struct Window {
    pub start: f32,
    pub fade_in: f32,
    pub end: f32,
    pub fade_out: f32,
}

impl Window {
    pub fn at(&self, t: f32) -> f32 {
        if t < self.start || t > self.end {
            return 0.0;
        }
        let mut f = 1.0;
        if t < self.start + self.fade_in {
            f = (t - self.start) / self.fade_in;
        }
        let fo = self.end - self.fade_out;
        if t > fo {
            f = f.min((self.end - t) / self.fade_out);
        }
        f.clamp(0.0, 1.0)
    }
}

/// Everything expensive that is shared across frames, built once.
pub struct NativeRenderer {
    pub data: SceneData,
    pub env: Option<AudioEnv>,
    pub cues: Vec<f32>,
    pub outro: Window,
    pub intro: Window,
    pub first_lyric: f32,
    /// Text colour of each block, linear — the sparks are tinted per block.
    pub block_colors: Vec<[f32; 3]>,
    /// Deterministic PRNG for the particle cloud (mulberry32, mirrors JS).
    particles: draw::ParticleCloud,
    /// Per-character meshes with their inflated hulls, plus each glyph's
    /// landing time for the follow-the-camera roll.
    pub prepared: Vec<draw::PreparedText>,
    streaks: draw::Streak,
    rings: [draw::RingSpec; 2],
    bars: draw::Bars,
    sparks: draw::Sparks,
}

impl NativeRenderer {
    pub fn new(data: SceneData, env: Option<AudioEnv>) -> Self {
        let cues = cue_list(&data);
        let duration = data.meta.total_frames as f32 / data.meta.fps as f32;
        // Cards are texts without char_times; the outro window opens on the
        // first card past the minute mark (318 in the bird PV).
        let outro_start = outro_start(&data);
        let outro = Window {
            start: outro_start,
            fade_in: 3.0,
            end: duration,
            fade_out: 2.0,
        };
        let first_lyric = cues
            .iter()
            .copied()
            .find(|c| *c > 8.0)
            .unwrap_or((duration * 0.1).min(16.5));
        let intro = Window {
            start: 2.0,
            fade_in: 1.6,
            end: (first_lyric - 0.7).max(6.0),
            fade_out: 1.3,
        };
        let block_colors = data
            .texts
            .iter()
            .map(|tx| {
                let c = match &tx.material {
                    crate::material::Material::Neon(n) => n.color,
                    crate::material::Material::Metal(m) => m.color,
                    crate::material::Material::Glass(g) => g.color,
                };
                [
                    srgb_to_linear(c.r),
                    srgb_to_linear(c.g),
                    srgb_to_linear(c.b),
                ]
            })
            .collect::<Vec<[f32; 3]>>();
        let particles = draw::ParticleCloud::new(data.meta.seed);
        let streaks = draw::Streak::new(data.meta.seed);
        let sparks = draw::Sparks::new(&data, &block_colors);
        let prepared = data
            .texts
            .iter()
            .map(|tx| {
                // Each glyph's landing time: the fade layer's delay (word
                // timing included) — what `buildText` resolves once in JS.
                let fade = tx.animations.iter().find_map(|a| match a {
                    AnimationSpec::Fade(f) => Some(f),
                    _ => None,
                });
                let chars = tx
                    .chars
                    .iter()
                    .map(|c| {
                        let lands_at = match fade {
                            Some(f) => {
                                match tx.char_times.as_deref().and_then(|ts| ts.get(c.idx)) {
                                    Some(&t) => t,
                                    None => f.start_at + c.idx as f32 * f.stagger,
                                }
                            }
                            None => 0.0,
                        };
                        let hull = tx
                            .material_outline_width()
                            .map(|w| draw::smooth_hull(&c.vertices, &c.normals, w));
                        let has_hull = hull.is_some();
                        draw::PreparedChar {
                            idx: c.idx,
                            base_position: c.base_position,
                            verts: c.vertices.clone(),
                            indices: c.indices.clone(),
                            hull_verts: hull.unwrap_or_default(),
                            hull_indices: if has_hull {
                                c.indices.clone()
                            } else {
                                Vec::new()
                            },
                            lands_at,
                        }
                    })
                    .collect();
                draw::PreparedText { chars }
            })
            .collect();
        Self {
            data,
            env,
            cues,
            outro,
            intro,
            first_lyric,
            block_colors,
            particles,
            prepared,
            streaks,
            rings: [
                draw::RingSpec::new(outro_ring(&outro)),
                draw::RingSpec::new(intro_ring(&intro)),
            ],
            bars: draw::Bars {
                center: Vec3::new(0.0, 0.8, -1.4),
                r0: 6.05,
                tilt: -0.1,
                max_len: 2.6,
                window: outro,
            },
            sparks,
        }
    }
}

fn outro_ring(w: &Window) -> draw::RingParams {
    draw::RingParams {
        center: Vec3::new(0.0, 0.8, -1.2),
        r0: 4.0,
        r1: 5.6,
        tilt: 0.0,
        hue_speed: 0.045,
        spin: 0.06,
        p0: 0.9,
        p1: 2.2,
        opacity: 0.95,
        window: *w,
    }
}

fn intro_ring(w: &Window) -> draw::RingParams {
    draw::RingParams {
        center: Vec3::new(0.0, 0.55, -1.9),
        r0: 2.9,
        r1: 4.3,
        tilt: -0.15,
        hue_speed: 0.03,
        spin: 0.05,
        p0: 0.7,
        p1: 1.7,
        opacity: 0.8,
        window: *w,
    }
}

impl crate::export::TextExport {
    fn material_outline_width(&self) -> Option<f32> {
        match &self.material {
            crate::material::Material::Neon(n) => n.outline.as_ref().map(|o| o.width),
            _ => None,
        }
    }
}

pub fn parse_hex(h: &str) -> (f32, f32, f32) {
    let h = h.trim_start_matches('#');
    let n = u32::from_str_radix(h, 16).unwrap_or(0xffffff);
    (
        ((n >> 16) & 255) as f32 / 255.0,
        ((n >> 8) & 255) as f32 / 255.0,
        (n & 255) as f32 / 255.0,
    )
}

/// Local Z rotation the follow-the-camera roll adds to a glyph landing under
/// a banked camera (port of the `mesh.rotateZ` block in `setFrame`).
pub fn follow_roll(cam_roll: f32, t: f32, lands_at: f32) -> f32 {
    if cam_roll == 0.0 {
        return 0.0;
    }
    let u = t - lands_at;
    if !(-0.02..1.05).contains(&u) {
        return 0.0;
    }
    let w = if u <= 0.45 {
        1.0
    } else {
        1.0 - smoothstep((u - 0.45) / 0.6)
    };
    if w > 0.001 {
        cam_roll * w
    } else {
        0.0
    }
}

/// Componentwise scale of a vertex array by a Vec3 scale, as a Vec3.
fn v3s(a: &[f32; 3], s: &Vec3) -> Vec3 {
    Vec3::new(a[0] * s.x, a[1] * s.y, a[2] * s.z)
}

pub fn quat_z(a: f32) -> Quat {
    Quat::from_axis_angle(Vec3::Z, a)
}

use std::io;

impl From<serde_json::Error> for crate::error::ChaminaError {
    fn from(e: serde_json::Error) -> Self {
        crate::error::ChaminaError::Other(format!("json: {e}"))
    }
}

pub fn load_scene_data(path: &Path) -> io::Result<SceneData> {
    let s = std::fs::read_to_string(path)?;
    Ok(serde_json::from_str(&s)?)
}

impl NativeRenderer {
    /// Duration in seconds from the frame counters.
    pub fn duration(&self) -> f32 {
        self.data.meta.total_frames as f32 / self.data.meta.fps as f32
    }

    /// Render one frame at `t` into a fresh linear HDR buffer.
    pub fn render_frame(&self, t: f32) -> draw::Frame {
        let _t0 = std::time::Instant::now();
        let mut stage = |name: &str, f: &mut dyn FnMut()| {
            let s = std::time::Instant::now();
            f();
            if std::env::var("NATIVE_PROFILE").is_ok() { eprintln!("[prof] {name}: {:?}", s.elapsed()); }
        };
        let mut stage = stage;
        let meta = &self.data.meta;
        let w = meta.width as usize;
        let h = meta.height as usize;
        let fov = meta.camera.fov;
        let aspect = w as f32 / h as f32;
        let near = meta.camera.near;

        let (bands, bass, energy) = match &self.env {
            Some(e) => e.sample(t),
            None => ([0.0; 64], 0.0, 0.0),
        };

        let fx = if meta.fx.enabled {
            Some(&meta.fx)
        } else {
            None
        };
        let orbit = match &meta.camera_animation {
            Some(crate::scene::CameraAnim::Orbit(o)) => Some(o),
            _ => None,
        };
        let cam = match orbit {
            Some(o) => update_camera(o, t, &self.cues, fx, energy, fov, aspect, near),
            None => {
                // No rig: the browser parks the camera at (0,1,26) → (0,1,0).
                let eye = Vec3::new(0.0, 1.0, 26.0);
                let target = Vec3::new(0.0, 1.0, 0.0);
                let fwd = (target - eye).normalize();
                let right = Vec3::new(-fwd.z, 0.0, fwd.x).normalize();
                let up = right.cross(fwd);
                Cam {
                    eye,
                    target,
                    fwd,
                    right,
                    up,
                    roll: 0.0,
                    tan_h: (fov * 0.5).tan(),
                    aspect,
                    near,
                }
            }
        };

        let mut f = draw::Frame::new(w, h);

        // --- skydome (renderOrder -1000 in the browser) -----------------------
        if let Some(pal) = eval_backgrounds(&meta.backgrounds, t) {
            stage("skydome", &mut || draw::draw_skydome(&mut f, &cam, t, &pal));
        }

        // --- text blocks, far to near (three sorts transparents by depth) -----
        stage("text+viz", &mut || {
        let mut order: Vec<usize> = (0..self.data.texts.len()).collect();
        order.sort_by(|&a, &b| {
            let da = (self.data.texts[a].position - cam.eye).length();
            let db = (self.data.texts[b].position - cam.eye).length();
            db.partial_cmp(&da).unwrap()
        });
        for &bi in &order {
            let tx = &self.data.texts[bi];
            let prep = &self.prepared[bi];
            let (neon_color, neon_strength, outline) = match &tx.material {
                crate::material::Material::Neon(n) => (
                    rgb_linear(n.color),
                    n.strength,
                    n.outline.map(|o| (rgb_linear(o.color), o.strength)),
                ),
                _ => continue, // the lyric stage is neon-only
            };
            let body = [
                neon_color[0] * neon_strength,
                neon_color[1] * neon_strength,
                neon_color[2] * neon_strength,
            ];
            for ch in &prep.chars {
                let st = animation::eval_all_timed(
                    &tx.animations,
                    t,
                    ch.idx,
                    tx.size,
                    tx.char_times.as_deref(),
                );
                let alpha = tx.alpha * st.alpha;
                if alpha <= 0.002 || st.scale <= 1e-3 {
                    continue;
                }
                let fr = follow_roll(cam.roll, t, ch.lands_at);
                let q = tx.rotation * st.rotation * Quat::from_axis_angle(Vec3::Z, fr);
                let s = Vec3::new(
                    tx.base_scale.x * st.scale,
                    tx.base_scale.y * st.scale,
                    tx.base_scale.z * st.scale,
                );
                let pos = ch.base_position + st.offset;
                // glyph body
                for tri in ch.indices.as_chunks::<3>().0 {
                    let a = pos + q.mul_vec3(v3s(&ch.verts[tri[0] as usize], &s));
                    let b = pos + q.mul_vec3(v3s(&ch.verts[tri[1] as usize], &s));
                    let c = pos + q.mul_vec3(v3s(&ch.verts[tri[2] as usize], &s));
                    f.tri(&cam, a, b, c, body, alpha, draw::Face::Both);
                }
                // inverted hull rim (BackSide only)
                if let Some((ocolor, ostrength)) = outline {
                    let rim = [
                        ocolor[0] * ostrength,
                        ocolor[1] * ostrength,
                        ocolor[2] * ostrength,
                    ];
                    for tri in ch.hull_indices.as_chunks::<3>().0 {
                        let a = pos + q.mul_vec3(v3s(&ch.hull_verts[tri[0] as usize], &s));
                        let b = pos + q.mul_vec3(v3s(&ch.hull_verts[tri[1] as usize], &s));
                        let c = pos + q.mul_vec3(v3s(&ch.hull_verts[tri[2] as usize], &s));
                        f.tri(&cam, a, b, c, rim, alpha, draw::Face::Back);
                    }
                }
            }
        }

        // --- audio-driven pieces ---------------------------------------------
        let orbit_base = orbit.map(|o| o.target).unwrap_or(Vec3::ZERO);
        let follow = match orbit {
            Some(o) => {
                let rig = eval_orbit(o, t);
                (rig.target - orbit_base) * 0.75
            }
            None => Vec3::ZERO,
        };
        // Dust colour + breathing (bass pump inside a ring window).
        let (pr, pg, pb) = parse_hex("#9fd8ff");
        let particle_color = [srgb_to_linear(pr), srgb_to_linear(pg), srgb_to_linear(pb)];
        let breath = self.intro.at(t).max(self.outro.at(t));
        let popacity = 0.9 * (1.0 + 0.28 * bass * breath);
        self.particles
            .draw(&mut f, &cam, t, follow, particle_color, popacity);

        if self.env.is_some() {
            let intro_f = self.intro.at(t);
            let outro_f = self.outro.at(t);
            for (ring, fade) in self.rings.iter().zip([outro_f, intro_f]) {
                ring.draw(&mut f, &cam, t, &bands, fade);
            }
            self.bars.draw(&mut f, &cam, t, &bands, outro_f);
            self.sparks.draw(&mut f, &cam, t, 0.9);
        }
        self.streaks.draw(&mut f, &cam, cam.roll, follow);
        });

        if std::env::var("NATIVE_PROFILE").is_ok() { eprintln!("[prof] total render_frame: {:?}", _t0.elapsed()); }
        f
    }
}
