//! Post chain in the browser's exact order: bloom → bokeh DOF → tonemap+sRGB
//! → FXAA → grade FX. Buffers stay linear HDR until the tonemap; FXAA and the
//! FX rack work on the finished sRGB picture like the browser passes do.

use rayon::prelude::*;

use super::draw::{hermite, Frame};

type Img = Vec<[f32; 3]>;

fn downsample(src: &Img, w: usize, h: usize) -> (Img, usize, usize) {
    let (nw, nh) = ((w / 2).max(1), (h / 2).max(1));
    let mut out = vec![[0.0; 3]; nw * nh];
    for y in 0..nh {
        for x in 0..nw {
            let (sx, sy) = (x * 2, y * 2);
            let mut acc = [0.0f32; 3];
            for dy in 0..2 {
                for dx in 0..2 {
                    let px = src[((sy + dy).min(h - 1)) * w + (sx + dx).min(w - 1)];
                    acc[0] += px[0];
                    acc[1] += px[1];
                    acc[2] += px[2];
                }
            }
            out[y * nw + x] = [acc[0] * 0.25, acc[1] * 0.25, acc[2] * 0.25];
        }
    }
    (out, nw, nh)
}

/// Separable 9-tap gaussian (sigma in destination pixels), one channel struct.
fn blur(src: &Img, w: usize, h: usize, sigma: f32) -> Img {
    let radius = (sigma * 2.5).ceil() as usize;
    let mut weights = Vec::with_capacity(radius + 1);
    for i in 0..=radius {
        weights.push((-((i * i) as f32) / (2.0 * sigma * sigma)).exp());
    }
    let sum: f32 = weights.iter().sum::<f32>() * 2.0 - weights[0];
    let weights: Vec<f32> = weights.into_iter().map(|v| v / sum).collect();

    let mut tmp = vec![[0.0f32; 3]; w * h];
    tmp.par_chunks_mut(w).enumerate().for_each(|(y, line)| {
        for (x, px) in line.iter_mut().enumerate() {
            let mut acc = [0.0f32; 3];
            for (i, wt) in weights.iter().enumerate() {
                let a = src[y * w + (x + i).min(w - 1)];
                let b = src[y * w + x.saturating_sub(i)];
                acc[0] += (a[0] + b[0]) * wt;
                acc[1] += (a[1] + b[1]) * wt;
                acc[2] += (a[2] + b[2]) * wt;
            }
            *px = acc;
        }
    });
    let mut out = vec![[0.0f32; 3]; w * h];
    out.par_chunks_mut(w).enumerate().for_each(|(y, line)| {
        for (x, px) in line.iter_mut().enumerate() {
            let mut acc = [0.0f32; 3];
            for (i, wt) in weights.iter().enumerate() {
                let a = src[(y + i).min(h - 1) * w + x];
                let b = src[y.saturating_sub(i) * w + x];
                acc[0] += (a[0] + b[0]) * wt;
                acc[1] += (a[1] + b[1]) * wt;
                acc[2] += (a[2] + b[2]) * wt;
            }
            *px = acc;
        }
    });
    out
}

fn upsample_to(src: &Img, sw: usize, sh: usize, out: &mut Img, w: usize, h: usize) {
    out.par_chunks_mut(w).enumerate().for_each(|(y, line)| {
        let fy = (y as f32 + 0.5) * sh as f32 / h as f32 - 0.5;
        let y0 = fy.floor().max(0.0) as usize;
        let y1 = (y0 + 1).min(sh - 1);
        let ty = (fy - y0 as f32).clamp(0.0, 1.0);
        for (x, px) in line.iter_mut().enumerate() {
            let fx = (x as f32 + 0.5) * sw as f32 / w as f32 - 0.5;
            let x0 = fx.floor().max(0.0) as usize;
            let x1 = (x0 + 1).min(sw - 1);
            let tx = (fx - x0 as f32).clamp(0.0, 1.0);
            let a = src[y0 * sw + x0];
            let b = src[y0 * sw + x1];
            let c = src[y1 * sw + x0];
            let d = src[y1 * sw + x1];
            for i in 0..3 {
                let top = a[i] + (b[i] - a[i]) * tx;
                let bot = c[i] + (d[i] - c[i]) * tx;
                px[i] = top + (bot - top) * ty;
            }
        }
    });
}

/// UnrealBloom equivalent: threshold bright-pass, a five-level half-res chain,
/// additive composite scaled by `intensity`.
pub fn bloom(f: &Frame, intensity: f32, threshold: f32) -> Img {
    let prof = std::env::var("NATIVE_PROFILE").is_ok();
    let lap = |name: &str, s: std::time::Instant| { if prof { eprintln!("  [prof] bloom.{name}: {:?}", s.elapsed()); } };
    let _b0 = std::time::Instant::now();
    let (w, h) = (f.w, f.h);
    // bright pass at half res
    let (mut cur, mut cw, mut ch) = {
        let (nw, nh) = ((w / 2).max(1), (h / 2).max(1));
        let mut b = vec![[0.0f32; 3]; nw * nh];
        b.par_chunks_mut(nw).enumerate().for_each(|(y, line)| {
            for (x, px) in line.iter_mut().enumerate() {
                let mut acc = [0.0f32; 3];
                for dy in 0..2 {
                    for dx in 0..2 {
                        // LuminosityHighPassShader: keep the *whole* texel where
                        // its luma clears the threshold (smooth knee of 0.01),
                        // black elsewhere — not (c - threshold).
                        let c = f.color[((y * 2 + dy).min(h - 1)) * w + (x * 2 + dx).min(w - 1)];
                        let luma = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
                        let alpha = hermite(((luma - threshold) / 0.01).clamp(0.0, 1.0));
                        for i in 0..3 {
                            acc[i] += c[i] * alpha;
                        }
                    }
                }
                *px = [acc[0] * 0.25, acc[1] * 0.25, acc[2] * 0.25];
            }
        });
        (b, nw, nh)
    };

    lap("brightpass", _b0);
    let _b1 = std::time::Instant::now();
    // mip chain: each level blurred with a widening kernel. UnrealBloom's
    // separable taps grow per mip, which is what throws the text's glow
    // hundreds of pixels across the frame at the low mips.
    let mut levels: Vec<Img> = Vec::new();
    let mut meta = Vec::new();
    for i in 0..5 {
        let sigma = (1u32 << i) as f32; // 1, 2, 4, 8, 16 at mip resolution
        cur = blur(&cur, cw, ch, sigma);
        levels.push(cur.clone());
        meta.push((cw, ch));
        if cw <= 2 || ch <= 2 {
            break;
        }
        let (n, nw, nh) = downsample(&cur, cw, ch);
        cur = n;
        cw = nw;
        ch = nh;
    }

    lap("mipchain", _b1);
    let _b2 = std::time::Instant::now();
    // upsample + accumulate
    let mut acc = vec![[0.0f32; 3]; w * h];
    let weights = [1.0f32, 0.8, 0.6, 0.4, 0.2];
    for (li, (lw, lh)) in meta.iter().copied().enumerate() {
        let mut up = vec![[0.0f32; 3]; w * h];
        upsample_to(&levels[li], lw, lh, &mut up, w, h);
        let wt = weights[li.min(weights.len() - 1)];
        let k = intensity * bloom_gain();
        acc.par_iter_mut().zip(&up).for_each(|(o, u)| {
            o[0] += u[0] * wt * k;
            o[1] += u[1] * wt * k;
            o[2] += u[2] * wt * k;
        });
    }
    lap("upsample+acc", _b2);
    acc
}

/// Bokeh-flavoured depth of field: the browser computes
/// `coc = clamp((focus - zc) * aperture, ±maxblur)` in UV units; we split the
/// frame into three blur levels and mix per pixel by coc, at half res.
pub fn dof(color: &Img, f: &Frame, focus: f32, aperture: f32, maxblur: f32) -> Img {
    let (w, h) = (f.w, f.h);
    let (hw, hh) = ((w / 2).max(1), (h / 2).max(1));
    // half-res colour + coc
    // (colour, coc) packed so one parallel pass fills both.
    let mut packed: Vec<([f32; 3], f32)> = vec![([0.0; 3], 0.0); hw * hh];
    packed.par_chunks_mut(hw).enumerate().for_each(|(y, line)| {
        for (x, px) in line.iter_mut().enumerate() {
            let mut acc = [0.0f32; 3];
            let mut cacc = 0.0f32;
            for dy in 0..2 {
                for dx in 0..2 {
                    let si = ((y * 2 + dy).min(h - 1)) * w + (x * 2 + dx).min(w - 1);
                    let c = color[si];
                    for i in 0..3 {
                        acc[i] += c[i] * 0.25;
                    }
                    let zc = f.depth[si];
                    let coc_uv = if zc.is_finite() {
                        ((focus - zc) * aperture).clamp(-maxblur, maxblur)
                    } else {
                        0.0
                    };
                    cacc += coc_uv.abs();
                }
            }
            *px = (acc, cacc * 0.25);
        }
    });
    let mut half: Vec<[f32; 3]> = packed.iter().map(|(c, _)| *c).collect();
    let coc: Vec<f32> = packed.iter().map(|(_, k)| *k).collect();
    let _ = &mut half;

    // Blur radii the scene actually spans (coc_px = |coc_uv| · width, capped
    // by maxblur): text ≈ 0, particles ~2 px, the dome at the 0.008 cap
    // ≈ 15 px.
    let b1 = blur(&half, hw, hh, 1.0);
    let b2 = blur(&half, hw, hh, 4.0);
    let b3 = blur(&half, hw, hh, 8.0);

    // coc in full-res pixels, normalised against the maxblur radius.
    let coc_px_per_uv = w as f32;
    let mut out = vec![[0.0f32; 3]; w * h];
    out.par_chunks_mut(w).enumerate().for_each(|(y, line)| {
        for (x, px) in line.iter_mut().enumerate() {
            let hx = (x / 2).min(hw - 1);
            let hy = (y / 2).min(hh - 1);
            let c01 = (coc[hy * hw + hx] * coc_px_per_uv / 15.0).clamp(0.0, 1.0);
            let src = f.color[y * w + x];
            // three-way mix, plus the sharp layer at low coc
            #[allow(clippy::many_single_char_names)]
            let (w0, w1, w2, w3) = if c01 < 0.33 {
                let k = c01 / 0.33;
                (1.0 - k, k, 0.0, 0.0)
            } else if c01 < 0.66 {
                let k = (c01 - 0.33) / 0.33;
                (0.0, 1.0 - k, k, 0.0)
            } else {
                let k = (c01 - 0.66) / 0.34;
                (0.0, 0.0, 1.0 - k, k)
            };
            let i = hy * hw + hx;
            for ch in 0..3 {
                px[ch] = src[ch] * w0 + b1[i][ch] * w1 + b2[i][ch] * w2 + b3[i][ch] * w3;
            }
        }
    });
    out
}

/// three's ACESFilmicToneMapping (Hill fit) + sRGB encode — the OutputPass.
pub fn tonemap_srgb(c: [f32; 3]) -> [f32; 3] {
    let m = [
        [0.59719, 0.07600, 0.02840],
        [0.35458, 0.90834, 0.13383],
        [0.04823, 0.01566, 0.83777],
    ];
    let mo = [
        [1.60475, -0.10208, -0.00327],
        [-0.53108, 1.10813, -0.07276],
        [-0.07367, -0.00605, 1.07602],
    ];
    let fit = |v: f32| -> f32 {
        let v = v.max(0.0);
        let a = v * (v + 0.0245786) - 0.000090537;
        let b = v * (0.983729 * v + 0.432_951) + 0.238081;
        a / b
    };
    let fitted = [
        fit(c[0] * m[0][0] + c[1] * m[0][1] + c[2] * m[0][2]),
        fit(c[0] * m[1][0] + c[1] * m[1][1] + c[2] * m[1][2]),
        fit(c[0] * m[2][0] + c[1] * m[2][1] + c[2] * m[2][2]),
    ];
    let mut out = [0.0f32; 3];
    for i in 0..3 {
        let v =
            (fitted[0] * mo[i][0] + fitted[1] * mo[i][1] + fitted[2] * mo[i][2]).clamp(0.0, 1.0);
        out[i] = if v <= 0.0031308 {
            v * 12.92
        } else {
            1.055 * v.powf(1.0 / 2.4) - 0.055
        };
    }
    out
}

/// Compact FXAA (the console variant): detect the luma edge from five taps and
/// blend along its gradient.
pub fn fxaa(src: &Img, w: usize, h: usize) -> Img {
    let luma = |c: [f32; 3]| 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
    let mut out = src.clone();
    out.par_chunks_mut(w).enumerate().for_each(|(y, line)| {
        for (x, px) in line.iter_mut().enumerate() {
            let get = |xx: i64, yy: i64| {
                src[yy.clamp(0, h as i64 - 1) as usize * w + xx.clamp(0, w as i64 - 1) as usize]
            };
            let c = get(x as i64, y as i64);
            let lc = luma(c);
            let ln = luma(get(x as i64, y as i64 - 1));
            let ls = luma(get(x as i64, y as i64 + 1));
            let lw = luma(get(x as i64 - 1, y as i64));
            let le = luma(get(x as i64 + 1, y as i64));
            let lmin = lc.min(ln).min(ls).min(lw).min(le);
            let lmax = lc.max(ln).max(ls).max(lw).max(le);
            let range = lmax - lmin;
            if range < 0.06 * lmax.max(0.15) {
                continue; // flat area
            }
            // vertical vs horizontal edge: blend across the smaller gradient
            let vert = (ln + ls).abs();
            let horz = (lw + le).abs();
            let dir = if vert > horz {
                (0.0f32, 1.0f32)
            } else {
                (1.0, 0.0)
            };
            let a = get(x as i64 - dir.0 as i64, y as i64 - dir.1 as i64);
            let b = get(x as i64 + dir.0 as i64, y as i64 + dir.1 as i64);
            for i in 0..3 {
                px[i] = (a[i] + b[i]) * 0.5;
            }
        }
    });
    out
}

/// The grade rack (chromatic aberration / saturation / vignette / cue flash).
pub fn grade(
    src: &Img,
    w: usize,
    h: usize,
    chromatic: f32,
    vignette: f32,
    saturation: f32,
    flash: f32,
) -> Img {
    let mut out = src.clone();
    out.par_chunks_mut(w).enumerate().for_each(|(y, line)| {
        for (x, px) in line.iter_mut().enumerate() {
            let u = (x as f32 + 0.5) / w as f32;
            let v = (y as f32 + 0.5) / h as f32;
            let cx = u - 0.5;
            let cy = v - 0.5;
            let r2 = cx * cx + cy * cy;
            let k = chromatic * (0.35 + r2 * 2.4);
            let sample = |du: f32, dv: f32| -> [f32; 3] {
                let sx = ((u + du).clamp(0.0, 1.0) * w as f32).min(w as f32 - 1.0) as usize;
                let sy = ((v + dv).clamp(0.0, 1.0) * h as f32).min(h as f32 - 1.0) as usize;
                src[sy * w + sx]
            };
            let off = (cx * k, cy * k);
            let r = sample(off.0, off.1)[0];
            let g = sample(0.0, 0.0)[1];
            let b = sample(-off.0, -off.1)[2];
            let mut col = [r, g, b];
            let l = 0.2126 * col[0] + 0.7152 * col[1] + 0.0722 * col[2];
            for c in &mut col {
                *c = l + (*c - l) * saturation;
            }
            let rad = ((cx * cx + cy * cy).sqrt() * 1.45).clamp(0.0, 1.0);
            for c in &mut col {
                *c *= 1.0 - vignette * rad * rad;
                *c += (1.0 - *c) * flash.clamp(0.0, 1.0);
            }
            *px = col;
        }
    });
    out
}

/// Linear HDR frame → sRGB bytes with the full post chain.
#[derive(Clone)]
pub struct PostConfig {
    pub bloom: Option<(f32, f32)>,    // intensity, threshold
    pub dof: Option<(f32, f32, f32)>, // focus, aperture, maxblur
    pub fxaa: bool,
    pub fx: Option<(f32, f32, f32, f32)>, // chromatic, vignette, saturation, flash
}

/// Calibration against the shipped browser look: UnrealBloom's composite
/// spreads wider than a naive mip sum; the gain was fitted to the reference
/// stills (see docs/cpp-migration.md M4 notes).
fn bloom_gain() -> f32 {
    std::env::var("NATIVE_BLOOM_GAIN")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(6.0)
}

pub fn finish(f: &Frame, cfg: &PostConfig) -> Vec<u8> {
    let prof = std::env::var("NATIVE_PROFILE").is_ok();
    let mut lap = |name: &str, s: std::time::Instant| {
        if prof { eprintln!("[prof] post.{name}: {:?}", s.elapsed()); }
    };
    let (w, h) = (f.w, f.h);
    let _l0 = std::time::Instant::now();
    // bloom + dof live on a snapshot (they read f.color / f.depth).
    let mut img: Img = f.color.clone();
    if let Some((intensity, threshold)) = cfg.bloom {
        let bl = bloom(f, intensity, threshold);
        img.par_iter_mut().zip(&bl).for_each(|(o, b)| {
            o[0] += b[0];
            o[1] += b[1];
            o[2] += b[2];
        });
    }
    lap("bloom", _l0);
    let _l1 = std::time::Instant::now();
    if let Some((focus, aperture, maxblur)) = cfg.dof {
        // Sequential like the browser: the bokeh blurs the *bloomed* buffer
        // (depth still comes from the frame).
        img = dof(&img, f, focus, aperture, maxblur);
    }
    lap("dof", _l1);
    let _l2 = std::time::Instant::now();
    let mut srgb: Img = img.iter().map(|c| tonemap_srgb(*c)).collect();
    if cfg.fxaa {
        srgb = fxaa(&srgb, w, h);
    }
    lap("tonemap+fxaa", _l2);
    let _l3 = std::time::Instant::now();
    if let Some((chromatic, vignette, saturation, flash)) = cfg.fx {
        srgb = grade(&srgb, w, h, chromatic, vignette, saturation, flash);
    }
    lap("grade", _l3);
    let mut bytes = vec![0u8; w * h * 3];
    for (i, c) in srgb.iter().enumerate() {
        bytes[i * 3] = (c[0].clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
        bytes[i * 3 + 1] = (c[1].clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
        bytes[i * 3 + 2] = (c[2].clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
    }
    bytes
}
