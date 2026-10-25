//! CPU rasteriser + the scene's drawn pieces (skydome, text meshes with their
//! inverted-hull rims, particle cloud, spectrum rings / bars, cue sparks, roll
//! streaks). Every formula is a line-for-line port of the GLSL in
//! `js/scene.js`; see the comment beside each piece for the source.

use rayon::prelude::*;

use crate::math::Vec3;

use super::{Cam, Window};

pub fn hermite(t: f32) -> f32 {
    t * t * (3.0 - 2.0 * t)
}

/// `smoothstep(e0, e1, x)` with the edges possibly inverted (the GLSL the
/// shaders lean on does this for falloffs; the clamp makes it well-defined).
pub fn smoothstep_edges(e0: f32, e1: f32, x: f32) -> f32 {
    hermite(((x - e0) / (e1 - e0)).clamp(0.0, 1.0))
}

/// Deterministic PRNG, bit-exact with `js/scene.js::mulberry32`.
pub struct Mulberry32(u32);

impl Mulberry32 {
    pub fn new(seed: u64) -> Self {
        Self(seed as u32)
    }
    pub fn next_f32(&mut self) -> f32 {
        self.0 = self.0.wrapping_add(0x6D2B79F5);
        let mut t = self.0;
        t = (t ^ (t >> 15)).wrapping_mul(t | 1);
        t ^= t.wrapping_add((t ^ (t >> 7)).wrapping_mul(t | 61));
        ((t ^ (t >> 14)) as f32) / 4294967296.0
    }
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

/// Linear HDR colour + view-depth. Text blends normally, everything glowing is
/// additive — exactly the browser's `EffectComposer` chain before the
/// tonemapping pass.
pub struct Frame {
    pub w: usize,
    pub h: usize,
    pub color: Vec<[f32; 3]>,
    pub depth: Vec<f32>,
}

impl Frame {
    pub fn new(w: usize, h: usize) -> Self {
        Self {
            w,
            h,
            color: vec![[0.0; 3]; w * h],
            depth: vec![f32::INFINITY; w * h],
        }
    }

    #[inline]
    fn idx(&self, x: i64, y: i64) -> usize {
        (y as usize) * self.w + x as usize
    }

    /// Screen coords `(sx, sy, zc)` of a world point, GL pixel convention
    /// (y up), or `None` behind the near plane.
    pub fn to_px(&self, cam: &Cam, p: Vec3) -> Option<(f32, f32, f32)> {
        let (xn, yn, zc) = cam.ndc(p)?;
        Some((
            (xn + 1.0) * 0.5 * self.w as f32,
            (1.0 - yn) * 0.5 * self.h as f32,
            zc,
        ))
    }

    /// One triangle, alpha-blended, depth-tested. `backface` culls
    /// front-facing triangles (the inverted hull keeps only its back faces).
    #[allow(clippy::too_many_arguments)]
    pub fn tri(
        &mut self,
        cam: &Cam,
        a: Vec3,
        b: Vec3,
        c: Vec3,
        color: [f32; 3],
        alpha: f32,
        face: Face,
    ) {
        let Some((ax, ay, az)) = self.to_px(cam, a) else {
            return;
        };
        let Some((bx, by, bz)) = self.to_px(cam, b) else {
            return;
        };
        let Some((cx, cy, cz)) = self.to_px(cam, c) else {
            return;
        };
        if az <= 0.0 || bz <= 0.0 || cz <= 0.0 {
            return;
        }
        if face != Face::Both {
            // World-space facing: n = (b-a)×(c-a) points toward the viewer for
            // a front face (the mesh winding is CCW-front, as the GPU assumes).
            let n = (b - a).cross(c - a);
            let centre = (a + b + c) * (1.0 / 3.0);
            let back = n.dot(cam.eye - centre) <= 0.0;
            if (face == Face::Back) != back {
                return;
            }
        }
        let area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if area.abs() < 1e-9 {
            return;
        }
        let inv = 1.0 / area;
        let x0 = ax.min(bx).min(cx).floor().max(0.0) as i64;
        let x1 = ax.max(bx).max(cx).ceil().min(self.w as f32 - 1.0) as i64;
        let y0 = ay.min(by).min(cy).floor().max(0.0) as i64;
        let y1 = ay.max(by).max(cy).ceil().min(self.h as f32 - 1.0) as i64;
        if x0 > x1 || y0 > y1 {
            return;
        }
        for py in y0..=y1 {
            for px in x0..=x1 {
                let x = px as f32 + 0.5;
                let y = py as f32 + 0.5;
                let w0 = ((bx - x) * (cy - y) - (by - y) * (cx - x)) * inv;
                let w1 = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) * inv;
                let w2 = 1.0 - w0 - w1;
                if w0 < 0.0 || w1 < 0.0 || w2 < 0.0 {
                    continue;
                }
                let zc = az * w0 + bz * w1 + cz * w2;
                let i = self.idx(px, py);
                if zc >= self.depth[i] {
                    continue;
                }
                self.depth[i] = zc;
                let s = self.color[i];
                self.color[i] = [
                    color[0] * alpha + s[0] * (1.0 - alpha),
                    color[1] * alpha + s[1] * (1.0 - alpha),
                    color[2] * alpha + s[2] * (1.0 - alpha),
                ];
            }
        }
    }

    /// One additive disc sprite. `glow` is the vertex-stage product (lit,
    /// shimmer, distance fade, window fade); the fragment's `a²` and the
    /// additive `×2` of the GLSL are applied here.
    pub fn disc(&mut self, cam: &Cam, center: Vec3, size_px: f32, color: [f32; 3], glow: f32) {
        let Some((sx, sy, zc)) = self.to_px(cam, center) else {
            return;
        };
        if zc <= 0.0 || size_px <= 0.0 {
            return;
        }
        let r = size_px * 0.5;
        let x0 = (sx - r).floor().max(0.0) as i64;
        let x1 = (sx + r).ceil().min(self.w as f32 - 1.0) as i64;
        let y0 = (sy - r).floor().max(0.0) as i64;
        let y1 = (sy + r).ceil().min(self.h as f32 - 1.0) as i64;
        if x0 > x1 || y0 > y1 {
            return;
        }
        // Depth test against the text: the sprites carry no depth of their own.
        let ci = self.idx(
            (sx as i64).clamp(0, self.w as i64 - 1),
            (sy as i64).clamp(0, self.h as i64 - 1),
        );
        if zc >= self.depth[ci] * 1.15 {
            return;
        }
        for py in y0..=y1 {
            for px in x0..=x1 {
                let dx = (px as f32 + 0.5 - sx) / size_px;
                let dy = (py as f32 + 0.5 - sy) / size_px;
                let d = (dx * dx + dy * dy).sqrt();
                let a = hermite((1.0 - 2.0 * d).clamp(0.0, 1.0));
                let alpha = a * a * glow;
                if alpha <= 0.004 {
                    continue;
                }
                let i = self.idx(px, py);
                let s = self.color[i];
                self.color[i] = [
                    s[0] + color[0] * alpha * 2.0,
                    s[1] + color[1] * alpha * 2.0,
                    s[2] + color[2] * alpha * 2.0,
                ];
            }
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Face {
    Both,
    Front,
    Back,
}

// ---------------------------------------------------------------------------
// skydome
// ---------------------------------------------------------------------------

fn hash(p: (f32, f32)) -> f32 {
    let s = (p.0 * 127.1 + p.1 * 311.7).sin() * 43_758.547;
    s - s.floor()
}

fn vnoise(p: (f32, f32)) -> f32 {
    let i = (p.0.floor(), p.1.floor());
    let f = (p.0 - i.0, p.1 - i.1);
    let u = (f.0 * f.0 * (3.0 - 2.0 * f.0), f.1 * f.1 * (3.0 - 2.0 * f.1));
    hash(i).mul_add(1.0 - u.0, hash((i.0 + 1.0, i.1)) * u.0) * (1.0 - u.1)
        + (hash((i.0, i.1 + 1.0)).mul_add(1.0 - u.0, hash((i.0 + 1.0, i.1 + 1.0)) * u.0)) * u.1
}

fn fbm(mut p: (f32, f32)) -> f32 {
    let (mut v, mut a) = (0.0f32, 0.5f32);
    for _ in 0..2 {
        v += a * vnoise(p);
        p = (p.0 * 2.03 + 1.7, p.1 * 2.03 + 9.2);
        a *= 0.5;
    }
    v
}

/// `buildBackground`'s fragment shader, one pixel at a time. `palette` is the
/// sRGB mix from `eval_backgrounds` (top, bottom, accent, nebula, stars,
/// drift); colours are linearised here as the JS `setRGB(…, SRGBColorSpace)`
/// does. `ray` is the normalised direction from the eye through the pixel.
#[allow(clippy::too_many_arguments)]
pub fn skydome_pixel(ray: Vec3, t: f32, palette: &[f32; 12]) -> [f32; 3] {
    let lin = |c: f32| super::srgb_to_linear(c);
    let top = [lin(palette[0]), lin(palette[1]), lin(palette[2])];
    let bottom = [lin(palette[3]), lin(palette[4]), lin(palette[5])];
    let accent = [lin(palette[6]), lin(palette[7]), lin(palette[8])];
    let (nebula, stars, drift) = (palette[9], palette[10], palette[11]);

    let d = ray;
    let h = (d.y * 0.5 + 0.5).clamp(0.0, 1.0);
    let mixv = h.powf(0.85);
    let mut col = [
        bottom[0] + (top[0] - bottom[0]) * mixv,
        bottom[1] + (top[1] - bottom[1]) * mixv,
        bottom[2] + (top[2] - bottom[2]) * mixv,
    ];

    let uv = (
        d.z.atan2(d.x) / std::f32::consts::TAU + 0.5,
        d.y * 0.5 + 0.5,
    );
    let q = (
        uv.0 * 7.0 + t * drift * 0.018,
        uv.1 * 3.5 + t * drift * 0.009,
    );
    let n = fbm((q.0 * 2.0, q.1 * 2.0));
    let cloud = smoothstep_edges(0.40, 0.92, n);
    let band = (-d.y.abs() * 5.0).exp();
    for i in 0..3 {
        col[i] += accent[i] * (cloud * nebula * 0.95 + band * nebula * 0.30);
    }

    if stars > 0.001 {
        let sp = (uv.0 * 380.0, uv.1 * 190.0);
        let cell = (sp.0.floor(), sp.1.floor());
        let r = hash(cell);
        let fp = (sp.0 - cell.0 - 0.5, sp.1 - cell.1 - 0.5);
        let jitter = (
            hash((cell.0 + 7.1, cell.1 + 7.1)) - 0.5,
            hash((cell.0 + 13.7, cell.1 + 13.7)) - 0.5,
        );
        let dd = ((fp.0 - jitter.0 * 0.6).powi(2) + (fp.1 - jitter.1 * 0.6).powi(2)).sqrt();
        let star = smoothstep_edges(0.16, 0.0, dd);
        let tw = 0.55 + 0.45 * (t * 1.7 + r * 40.0).sin();
        let on = if r <= 0.055 { 1.0 } else { 0.0 };
        for (col_i, c) in [0.78f32, 0.86, 1.0].iter().enumerate() {
            col[col_i] += c * star * on * tw * stars * 1.7;
        }
    }

    // Sink the corners a little so the frame reads as a lit stage.
    let rad = ((d.x * d.x + d.y * d.y).sqrt() * 1.15).clamp(0.0, 1.0);
    let k = 1.0 - 0.30 * rad * rad;
    [col[0] * k, col[1] * k, col[2] * k]
}

/// Fill the whole frame with the dome. Parallel rows; the dome never writes
/// depth (the browser's dome has `depthWrite: false`).
pub fn draw_skydome(f: &mut Frame, cam: &Cam, t: f32, palette: &[f32; 12]) {
    let w = f.w;
    let h = f.h;
    let cam = Cam {
        eye: cam.eye,
        target: cam.target,
        fwd: cam.fwd,
        right: cam.right,
        up: cam.up,
        roll: cam.roll,
        tan_h: cam.tan_h,
        aspect: cam.aspect,
        near: cam.near,
    };
    let pal: &[f32; 12] = palette;
    let color = &mut f.color;
    let depth = &mut f.depth;
    color
        .par_chunks_mut(w)
        .zip(depth.par_chunks_mut(w))
        .enumerate()
        .for_each(|(row, (line, dline))| {
            let py = row as f32 + 0.5;
            let yn = 1.0 - 2.0 * py / h as f32;
            for (col_i, px) in line.iter_mut().enumerate() {
                let xn = 2.0 * (col_i as f32 + 0.5) / w as f32 - 1.0;
                // cam.right / cam.up already carry the roll.
                let ray = (cam.fwd
                    + cam.right * (xn * cam.tan_h * cam.aspect)
                    + cam.up * (yn * cam.tan_h))
                    .normalize();
                *px = skydome_pixel(ray, t, pal);
                // The browser's depth override pass sees the dome at its real
                // distance; that far depth is what drives the heavy bokeh on
                // the sky. Everything drawn later overwrites it.
                dline[col_i] = 300.0;
            }
        });
}

// ---------------------------------------------------------------------------
// particle cloud
// ---------------------------------------------------------------------------

pub struct ParticleCloud {
    /// (offset xyz, phase, speed, scale)
    pub p: Vec<([f32; 3], f32, f32, f32)>,
}

impl ParticleCloud {
    pub fn new(seed: u64) -> Self {
        let mut r = Mulberry32::new(seed);
        let mut p = Vec::with_capacity(2600);
        for _ in 0..2600 {
            p.push((
                [
                    (r.next_f32() - 0.5) * 70.0,
                    r.next_f32() * 70.0 - 25.0,
                    (r.next_f32() - 0.5) * 45.0 - 20.0,
                ],
                r.next_f32() * std::f32::consts::TAU,
                0.25 + r.next_f32() * 1.4,
                0.6 + r.next_f32() * 2.4,
            ));
        }
        Self { p }
    }

    pub fn draw(
        &self,
        f: &mut Frame,
        cam: &Cam,
        t: f32,
        offset: Vec3,
        color: [f32; 3],
        opacity: f32,
    ) {
        for (off, phase, speed, scale) in &self.p {
            let y = ((off[1] + t * speed + 45.0).rem_euclid(70.0)) - 25.0;
            let center = offset + Vec3::new(off[0], y, off[2]);
            let Some((_, _, zc)) = cam.view(center) else {
                continue;
            };
            let size = (*scale * (260.0 / zc.max(0.001))).min(46.0);
            let mut tw = 0.45 + 0.55 * (t * 2.1 + phase).sin();
            tw *= smoothstep_edges(2.0, 7.0, zc);
            f.disc(cam, center, size, color, tw * opacity);
        }
    }
}

// ---------------------------------------------------------------------------
// spectrum rings
// ----------------------------------------------------------------///

pub struct RingSpec {
    pub center: Vec3,
    pub r0: f32,
    pub r1: f32,
    pub tilt: f32,
    pub hue_speed: f32,
    pub spin: f32,
    pub p0: f32,
    pub p1: f32,
    pub opacity: f32,
    pub window: Window,
    pub pts: Vec<(Vec3, f32, f32)>, // (position around origin, band, k)
}

impl RingSpec {
    pub fn new(spec: RingParams) -> Self {
        let bands = 64;
        let radial = 7;
        let mut pts = Vec::with_capacity(bands * radial);
        for b in 0..bands {
            let ang =
                -std::f32::consts::FRAC_PI_2 + (b as f32 / bands as f32) * std::f32::consts::TAU;
            let (dx, dy) = (ang.cos(), ang.sin());
            for k in 0..radial {
                let r = spec.r0 + (spec.r1 - spec.r0) * k as f32 / (radial - 1) as f32;
                pts.push((Vec3::new(dx * r, dy * r, 0.0), b as f32, k as f32));
            }
        }
        Self {
            center: spec.center,
            r0: spec.r0,
            r1: spec.r1,
            tilt: spec.tilt,
            hue_speed: spec.hue_speed,
            spin: spec.spin,
            p0: spec.p0,
            p1: spec.p1,
            opacity: spec.opacity,
            window: spec.window,
            pts,
        }
    }

    pub fn draw(&self, f: &mut Frame, cam: &Cam, t: f32, bands: &[f32; 64], fade: f32) {
        if fade <= 0.003 {
            return;
        }
        let spin = t * self.spin;
        let (ca, sa) = (spin.cos(), spin.sin());
        // object rotation: spin about Z in ring space, then tilt about X.
        let (ct, st) = (self.tilt.cos(), self.tilt.sin());
        for (pos, band, k) in &self.pts {
            let pr = Vec3::new(pos.x * ca - pos.y * sa, pos.x * sa + pos.y * ca, pos.z);
            // rotateX(tilt): y' = y*c - z*s, z' = y*s + z*c
            let pw = Vec3::new(pr.x, pr.y * ct - pr.z * st, pr.y * st + pr.z * ct) + self.center;
            let e = bands[*band as usize];
            let lvl = e * 7.0;
            let lit = smoothstep_edges(*k - 0.45, *k + 0.05, lvl);
            let shim = 0.5 + 0.5 * (t * 1.7 + band * 1.53).sin();
            let size = (self.p0 + (self.p1 - self.p0) * lit) * (0.75 + 0.45 * e);
            let Some((_, _, zc)) = cam.view(pw) else {
                continue;
            };
            let size_px = (size * (260.0 / zc.max(0.001))).min(44.0);
            let mut glow = 0.16 + 0.84 * lit * (0.72 + 0.28 * shim);
            glow *= smoothstep_edges(2.0, 7.0, zc);
            glow *= fade;
            let hue = (*band / 64.0 + t * self.hue_speed).fract();
            let vcolor = hsv2rgb(hue, 0.85, 0.30 + 0.70 * lit);
            f.disc(cam, pw, size_px, vcolor, glow * self.opacity);
        }
    }
}

pub struct RingParams {
    pub center: Vec3,
    pub r0: f32,
    pub r1: f32,
    pub tilt: f32,
    pub hue_speed: f32,
    pub spin: f32,
    pub p0: f32,
    pub p1: f32,
    pub opacity: f32,
    pub window: Window,
}

/// GLSL `hsv2rgb` from the ring shader:
/// `p = abs(fract(c.xxx + K) * 6 - 3); rgb = z * mix(1, clamp(p-1,0,1), y)`.
pub fn hsv2rgb(h: f32, s: f32, v: f32) -> [f32; 3] {
    let k = [1.0, 2.0 / 3.0, 1.0 / 3.0];
    let mut out = [0.0f32; 3];
    for (i, ki) in k.iter().enumerate() {
        let p = ((h + ki).fract() * 6.0 - 3.0).abs();
        let p = (p - 1.0).clamp(0.0, 1.0);
        out[i] = v * (1.0 - s * p);
    }
    out
}

/// three's `Color.setHSL` (sRGB) → linear RGB, for the spectrum bars.
pub fn hsl_to_linear(h: f32, s: f32, l: f32) -> [f32; 3] {
    let h = h.fract();
    let f = |n: f32| {
        let k = (n + h * 12.0).rem_euclid(12.0);
        let a = s * l.min(1.0 - l);
        let v = l - a * (-1.0 + (k - 3.0).abs().min(9.0 - k).min(1.0));
        super::srgb_to_linear(v.clamp(0.0, 1.0))
    };
    [f(0.0), f(8.0), f(4.0)]
}

// ---------------------------------------------------------------------------
// spectrum bars (instanced boxes)
// ----------------------------------------------------------------///

pub struct Bars {
    pub center: Vec3,
    pub r0: f32,
    pub tilt: f32,
    pub max_len: f32,
    pub window: Window,
}

const BOX_VERTS: [[f32; 3]; 8] = [
    [0.0, -0.5, -0.5],
    [1.0, -0.5, -0.5],
    [1.0, 0.5, -0.5],
    [0.0, 0.5, -0.5],
    [0.0, -0.5, 0.5],
    [1.0, -0.5, 0.5],
    [1.0, 0.5, 0.5],
    [0.0, 0.5, 0.5],
];

const BOX_TRIS: [[usize; 3]; 12] = [
    [0, 2, 1],
    [0, 3, 2],
    [4, 5, 6],
    [4, 6, 7],
    [0, 1, 5],
    [0, 5, 4],
    [2, 3, 7],
    [2, 7, 6],
    [1, 2, 6],
    [1, 6, 5],
    [3, 0, 4],
    [3, 4, 7],
];

impl Bars {
    pub fn draw(&self, f: &mut Frame, cam: &Cam, t: f32, bands: &[f32; 64], fade: f32) {
        if fade <= 0.003 {
            return;
        }
        let opacity = 0.9 * fade;
        let (ct, st) = (self.tilt.cos(), self.tilt.sin());
        #[allow(clippy::needless_range_loop)]
        for b in 0..64usize {
            let v = bands[b];
            let ang =
                -std::f32::consts::FRAC_PI_2 + (b as f32 / 64.0) * std::f32::consts::TAU + t * 0.05;
            let sx = 0.22 + v * self.max_len;
            let hue = ((b as f32 / 64.0) + t * 0.02).fract();
            let color = hsl_to_linear(hue, 0.85, 0.22 + 0.5 * v);
            let (ca, sa) = (ang.cos(), ang.sin());
            for tri in BOX_TRIS {
                let mut world = [Vec3::ZERO; 3];
                for (vi, &idx) in tri.iter().enumerate() {
                    let v0 = BOX_VERTS[idx];
                    // instance: scale, rotZ, translate to the ring
                    let lx = v0[0] * sx;
                    let ly = v0[1] * 0.11;
                    let _lz = v0[2] * 0.11; // viewed face-on: the z-extent folds into the tilt
                    let (x, y) = (lx * ca - ly * sa, lx * sa + ly * ca);
                    let px = x + ang.cos() * self.r0;
                    let py = y + ang.sin() * self.r0;
                    // group: rotX(tilt) then translate
                    let wx = px;
                    let wy = py * ct;
                    let wz = py * st;
                    world[vi] = self.center + Vec3::new(wx, wy, wz);
                }
                f.tri(
                    cam,
                    world[0],
                    world[1],
                    world[2],
                    color,
                    opacity,
                    Face::Both,
                );
            }
        }
    }
}

// ---------------------------------------------------------------------------
// cue sparks
// ----------------------------------------------------------------///

pub struct Sparks {
    /// (position, cue, color linear, seed0, seed1, seed2)
    pub p: Vec<(Vec3, f32, [f32; 3], f32, f32, f32)>,
}

impl Sparks {
    pub fn new(data: &super::SceneData, block_colors: &[[f32; 3]]) -> Self {
        let mut p = Vec::new();
        for (bi, tx) in data.texts.iter().enumerate() {
            let Some(fade) = tx.animations.iter().find_map(|a| match a {
                crate::animation::AnimationSpec::Fade(f) => Some(f.start_at),
                _ => None,
            }) else {
                continue;
            };
            let pos = tx.position;
            let hw = (tx.total_width_em * tx.size * 0.5).clamp(1.0, 9.0);
            let mut r = Mulberry32::new((bi as u64).wrapping_mul(131).wrapping_add(7));
            for _ in 0..14 {
                p.push((
                    Vec3::new(
                        pos.x + (r.next_f32() * 2.0 - 1.0) * hw * 1.05,
                        pos.y + (r.next_f32() * 2.0 - 1.0) * 0.85,
                        pos.z + (r.next_f32() - 0.5) * 0.6,
                    ),
                    fade + r.next_f32() * 0.15,
                    block_colors[bi],
                    std::f32::consts::PI * (0.15 + 0.7 * r.next_f32()),
                    0.5 + r.next_f32() * 1.6,
                    2.0 + r.next_f32() * 4.0,
                ));
            }
        }
        Self { p }
    }

    pub fn draw(&self, f: &mut Frame, cam: &Cam, t: f32, opacity: f32) {
        for (pos, cue, color, s0, s1, s2) in &self.p {
            let u = (t - cue) / 1.15;
            if !(0.0..1.0).contains(&u) {
                continue;
            }
            let e = (1.0 - u) * (1.0 - u);
            let drift = Vec3::new(s0.cos(), s0.sin(), 0.0) * (s1 * u * 1.5);
            let center = *pos + drift + Vec3::new(0.0, u * 0.35, 0.0);
            let Some((_, _, zc)) = cam.view(center) else {
                continue;
            };
            let size = s2 * (0.45 + 0.55 * e);
            let size_px = (size * (240.0 / zc.max(0.001))).min(26.0);
            f.disc(cam, center, size_px, *color, e * e * opacity);
        }
    }
}

// ---------------------------------------------------------------------------
// roll streaks
// ----------------------------------------------------------------///

pub type StreakQuad = ([f32; 2], [f32; 2], [f32; 2], f32, f32, f32);

pub struct Streak {
    /// (cx, cy, tangent, normal, len, width, ring01)
    pub quads: Vec<StreakQuad>,
}

impl Streak {
    pub fn new(seed: u64) -> Self {
        let mut r = Mulberry32::new(seed ^ 0x9E3779B9);
        let mut quads = Vec::with_capacity(26);
        for i in 0..26 {
            let th = (i as f32 / 26.0) * std::f32::consts::TAU + (r.next_f32() - 0.5) * 0.35;
            let rad = 3.6 + r.next_f32() * 5.2;
            let len = 1.6 + r.next_f32() * 2.8;
            let wid = 0.022 + r.next_f32() * 0.035;
            let c = [th.cos() * rad, th.sin() * rad];
            let t = [-th.sin(), th.cos()];
            let n = [th.cos(), th.sin()];
            quads.push((c, t, n, len, wid, (rad - 3.6) / 5.2));
        }
        Self { quads }
    }

    pub fn draw(&self, f: &mut Frame, cam: &Cam, roll: f32, offset: Vec3) {
        let opacity = ((roll.abs() - 0.22) / 0.9).clamp(0.0, 1.0) * 0.85;
        if opacity <= 0.01 {
            return;
        }
        // group at followOffset + (0,0,-0.7), mesh rotZ = roll * 0.92
        let rz = roll * 0.92;
        let (cr, sr) = (rz.cos(), rz.sin());
        let origin = offset + Vec3::new(0.0, 0.0, -0.7);
        for (c, t, n, len, wid, ring) in &self.quads {
            // rotate the quad's centre + axes by rz
            let c2 = [c[0] * cr - c[1] * sr, c[0] * sr + c[1] * cr];
            let t2 = [t[0] * cr - t[1] * sr, t[0] * sr + t[1] * cr];
            let n2 = [n[0] * cr - n[1] * sr, n[0] * sr + n[1] * cr];
            let centre = origin + Vec3::new(c2[0], c2[1], 0.0);
            let col_near = [0.82f32, 0.93, 1.0];
            let col_far = [0.45f32, 0.75, 1.0];
            let color = [
                col_near[0] + (col_far[0] - col_near[0]) * ring,
                col_near[1] + (col_far[1] - col_near[1]) * ring,
                col_near[2] + (col_far[2] - col_near[2]) * ring,
            ];
            // Analytic quad fill: per pixel, vT = 2·(rel·t)/len and
            // vN = 2·(rel·n)/wid; fragment a = (1-|vT|)²·uOpacity·(0.3+0.5 vC).
            let quad_half_t = len * 0.5;
            let quad_half_n = wid * 0.5;
            let mut corners = [Vec3::ZERO; 4];
            for (vi, (st, sn)) in [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)]
                .iter()
                .enumerate()
            {
                corners[vi] = centre
                    + Vec3::new(t2[0], t2[1], 0.0) * (st * quad_half_t)
                    + Vec3::new(n2[0], n2[1], 0.0) * (sn * quad_half_n);
            }
            let mut xs = [0f32; 4];
            let mut ys = [0f32; 4];
            let mut zs = [0f32; 4];
            let mut ok = true;
            for vi in 0..4 {
                match f.to_px(cam, corners[vi]) {
                    Some((x, y, z)) => {
                        xs[vi] = x;
                        ys[vi] = y;
                        zs[vi] = z;
                    }
                    None => {
                        ok = false;
                        break;
                    }
                }
            }
            if !ok {
                continue;
            }
            let x0 = xs[0].min(xs[1]).min(xs[2]).min(xs[3]).floor().max(0.0) as i64;
            let x1 = xs[0]
                .max(xs[1])
                .max(xs[2])
                .max(xs[3])
                .ceil()
                .min(f.w as f32 - 1.0) as i64;
            let y0 = ys[0].min(ys[1]).min(ys[2]).min(ys[3]).floor().max(0.0) as i64;
            let y1 = ys[0]
                .max(ys[1])
                .max(ys[2])
                .max(ys[3])
                .ceil()
                .min(f.h as f32 - 1.0) as i64;
            if x0 > x1 || y0 > y1 {
                continue;
            }
            for py in y0..=y1 {
                for px in x0..=x1 {
                    let wpx = px as f32 + 0.5;
                    let wpy = py as f32 + 0.5;
                    // Unproject the pixel into a ray and intersect the quad's
                    // plane (z = centre.z — the quads lie in an XY plane).
                    let ndc_x = 2.0 * wpx / f.w as f32 - 1.0;
                    let ndc_y = 1.0 - 2.0 * wpy / f.h as f32;
                    let dir = (cam.fwd
                        + cam.right * (ndc_x * cam.tan_h * cam.aspect)
                        + cam.up * (ndc_y * cam.tan_h))
                        .normalize();
                    if dir.z.abs() < 1e-6 {
                        continue;
                    }
                    let tt = (centre.z - cam.eye.z) / dir.z;
                    if tt <= 0.0 {
                        continue;
                    }
                    let p = cam.eye + dir * tt;
                    let rel = p - centre;
                    let vt = (rel.x * t2[0] + rel.y * t2[1]) / (len * 0.5);
                    let vn = (rel.x * n2[0] + rel.y * n2[1]) / (wid * 0.5);
                    if vt.abs() > 1.0 || vn.abs() > 1.0 {
                        continue;
                    }
                    let i = f.idx(px, py);
                    if tt >= f.depth[i] {
                        continue;
                    }
                    let alpha = (1.0 - vt.abs()).powi(2) * opacity * (0.3 + 0.5 * ring);
                    let s = f.color[i];
                    f.color[i] = [
                        s[0] + color[0] * alpha * 1.6,
                        s[1] + color[1] * alpha * 1.6,
                        s[2] + color[2] * alpha * 1.6,
                    ];
                }
            }
        }
    }
}

pub type SceneDataRef<'a> = &'a super::SceneData;

/// The text mesh of one character plus its inverted hull, prepared once.
pub struct PreparedChar {
    pub idx: usize,
    pub base_position: Vec3,
    pub verts: Vec<[f32; 3]>,
    pub indices: Vec<u32>,
    pub hull_verts: Vec<[f32; 3]>,
    pub hull_indices: Vec<u32>,
    pub lands_at: f32,
}

pub struct PreparedText {
    pub chars: Vec<PreparedChar>,
}

/// Port of `smoothHullGeometry`: average normals across coincident positions,
/// then inflate along the averaged normal by `width` (em units, local space).
pub fn smooth_hull(verts: &[[f32; 3]], normals: &[[f32; 3]], width: f32) -> Vec<[f32; 3]> {
    let key = |p: [f32; 3]| -> (i64, i64, i64) {
        (
            (p[0] * 8192.0).round() as i64,
            (p[1] * 8192.0).round() as i64,
            (p[2] * 8192.0).round() as i64,
        )
    };
    let mut acc: std::collections::HashMap<(i64, i64, i64), [f64; 3]> =
        std::collections::HashMap::new();
    for (p, n) in verts.iter().zip(normals.iter()) {
        let e = acc.entry(key(*p)).or_insert([0.0; 3]);
        e[0] += n[0] as f64;
        e[1] += n[1] as f64;
        e[2] += n[2] as f64;
    }
    verts
        .iter()
        .map(|p| {
            let e = &acc[&key(*p)];
            let l = ((e[0] * e[0] + e[1] * e[1] + e[2] * e[2]).sqrt()) as f32;
            let l = if l == 0.0 { 1.0 } else { l };
            [
                p[0] + (e[0] as f32 / l) * width,
                p[1] + (e[1] as f32 / l) * width,
                p[2] + (e[2] as f32 / l) * width,
            ]
        })
        .collect()
}
