//! Declarative animation value types + the per-character evaluation used by
//! the exporters.
//!
//! These are plain data — no ECS, no renderer. They describe *what* moves;
//! [`eval_all`] turns them into an [`AnimState`] at a given time, and the
//! JS/Three.js renderer mirrors this exact arithmetic frame by frame.
//!
//! Every animation can be applied to a whole text or, when it carries a
//! `stagger`, to each character independently (per-character index drives the
//! delay, producing the classic wave / cascade entering effect).

use std::f32::consts::{SQRT_2, TAU};

use serde::{Deserialize, Serialize};

use crate::math::{Quat, Vec3};

/// Smoothstep on `[0, 1]`; identical to [`Ease::Smooth`] but usable as a plain
/// value (envelopes, ramps) without building an easing enum.
fn smoothstep(x: f32) -> f32 {
    let x = x.clamp(0.0, 1.0);
    x * x * (3.0 - 2.0 * x)
}

/// Per-axis detuned harmonic and starting phase for [`jitter_noise`].
const HARMONIC: [f32; 3] = [1.618_034, SQRT_2, 1.732_050_8];
const AXIS_PHASE: [f32; 3] = [0.0, 2.094_395_1, 4.188_790_2];
/// Golden angle between neighbouring characters, so a line jitters as noise
/// rather than as a coherent wave.
const CHAR_PHASE: f32 = 2.399_963_2;

/// Two detuned sines per axis: smooth, unbounded, and cheap enough to evaluate
/// for every character of every visible line each frame.
///
/// Sines rather than a hash **on purpose** — `f32::sin` and `Math.sin` agree to
/// rounding, while a seeded hash would diverge between the Rust reference and
/// the JS renderer after the first frame.
pub fn jitter_noise(t: f32, w: f32, p: f32) -> Vec3 {
    let mut o = [0.0_f32; 3];
    for (i, slot) in o.iter_mut().enumerate() {
        let ph = p + AXIS_PHASE[i];
        *slot = 0.5 * (w * t + ph).sin() + 0.5 * (w * HARMONIC[i] * t + ph * 1.7).sin();
    }
    Vec3::new(o[0], o[1], o[2])
}

// ---------------------------------------------------------------------------
// Easing
// ---------------------------------------------------------------------------

/// Simple easing functions (input in `[0, 1]`, output `[0, 1]`).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Ease {
    Linear,
    Smooth,
    EaseInQuad,
    EaseOutQuad,
    EaseInOutCubic,
    BackOut,
}

impl Ease {
    pub fn apply(self, x: f32) -> f32 {
        let x = x.clamp(0.0, 1.0);
        match self {
            Ease::Linear => x,
            Ease::Smooth => x * x * (3.0 - 2.0 * x),
            Ease::EaseInQuad => x * x,
            Ease::EaseOutQuad => 1.0 - (1.0 - x) * (1.0 - x),
            Ease::EaseInOutCubic => {
                if x < 0.5 {
                    4.0 * x * x * x
                } else {
                    1.0 - (-2.0 * x + 2.0).powi(3) / 2.0
                }
            }
            Ease::BackOut => {
                const C1: f32 = 1.70158;
                const C3: f32 = C1 + 1.0;
                1.0 + C3 * (x - 1.0).powi(3) + C1 * (x - 1.0).powi(2)
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Fall
// ---------------------------------------------------------------------------

/// Each character drops from `height` above its resting spot with a
/// deterministic ballistic profile (gravity + initial velocity chosen so the
/// char lands exactly at `t = duration`). `stagger` delays successive chars.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Fall {
    pub duration: f32,
    pub stagger: f32,
    pub height: f32,
    /// Downward acceleration in units/s^2 (negative = down).
    pub gravity: f32,
    /// Horizontal drift in units/s (positive = +X).
    pub drift: f32,
    /// Extra Z wobble amplitude.
    pub wobble: f32,
    /// Absolute time (s) the drop begins. Characters are delayed by
    /// `start_at + idx * stagger`, and hang at `height` until then — so a
    /// scheduled line stays parked off-frame until its cue.
    pub start_at: f32,
}

impl Default for Fall {
    fn default() -> Self {
        Self {
            duration: 2.0,
            stagger: 0.1,
            height: 4.0,
            gravity: -12.0,
            drift: 0.0,
            wobble: 0.0,
            start_at: 0.0,
        }
    }
}

impl Fall {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn duration(mut self, d: f32) -> Self {
        self.duration = d;
        self
    }

    pub fn stagger(mut self, s: f32) -> Self {
        self.stagger = s;
        self
    }

    pub fn height(mut self, h: f32) -> Self {
        self.height = h;
        self
    }

    pub fn gravity(mut self, g: f32) -> Self {
        self.gravity = g;
        self
    }

    pub fn drift(mut self, d: f32) -> Self {
        self.drift = d;
        self
    }

    pub fn start_at(mut self, t: f32) -> Self {
        self.start_at = t;
        self
    }

    /// Vertical offset at normalized time `u` in `[0, 1]` of a ballistic drop
    /// that starts at `height` and ends at 0 exactly at `u = 1`.
    pub fn offset_at(&self, u: f32) -> f32 {
        let u = u.clamp(0.0, 1.0);
        let t = self.duration * u;
        // Solve v0 from: height + v0*t_end + 0.5*g*t_end^2 = 0
        let v0 =
            -(self.height + 0.5 * self.gravity * self.duration * self.duration) / self.duration;
        self.height + v0 * t + 0.5 * self.gravity * t * t
    }
}

// ---------------------------------------------------------------------------
// Fade
// ---------------------------------------------------------------------------

/// Per-character fade-in (alpha ramp). Optional fade-out so the sequence can
/// end cleanly. `duration` is the fade-in duration.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Fade {
    pub duration: f32,
    pub stagger: f32,
    /// Absolute time (s) the fade-in begins (lyric cue). Characters ramp from
    /// `start_at + idx * stagger`, so before the cue the text is fully hidden.
    pub start_at: f32,
    /// If set: fade-out starts at this absolute time (seconds).
    #[serde(default)]
    pub fade_out_at: Option<f32>,
    /// Fade-out duration from `fade_out_at`.
    pub fade_out: f32,
}

impl Default for Fade {
    fn default() -> Self {
        Self {
            duration: 0.8,
            stagger: 0.1,
            start_at: 0.0,
            fade_out_at: None,
            fade_out: 1.0,
        }
    }
}

impl Fade {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn duration(mut self, d: f32) -> Self {
        self.duration = d;
        self
    }

    pub fn stagger(mut self, s: f32) -> Self {
        self.stagger = s;
        self
    }

    pub fn start_at(mut self, t: f32) -> Self {
        self.start_at = t;
        self
    }

    /// The opacity factor (0 = invisible, 1 = full) at time `t` for the
    /// character whose entry is delayed by `delay` seconds.
    pub fn factor_at(&self, t: f32, delay: f32) -> f32 {
        let t0 = delay;
        let mut f = 1.0;
        if t < t0 {
            f = 0.0;
        } else if t < t0 + self.duration {
            f = (t - t0) / self.duration.max(1e-6);
        }
        if let Some(fo) = self.fade_out_at {
            let fin = fo + self.fade_out;
            if t > fo {
                f *= (1.0 - ((t - fo) / self.fade_out.max(1e-6))).clamp(0.0, 1.0);
                if t >= fin {
                    f = 0.0;
                }
            }
        }
        f.clamp(0.0, 1.0)
    }
}

// ---------------------------------------------------------------------------
// Spin
// ---------------------------------------------------------------------------

/// Continuous rotation about an axis.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Spin {
    pub speed: f32,
    pub axis: Vec3,
    pub start_delay: f32,
}

impl Default for Spin {
    fn default() -> Self {
        Self {
            speed: 0.8,
            axis: Vec3::Y,
            start_delay: 0.0,
        }
    }
}

impl Spin {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn speed(mut self, s: f32) -> Self {
        self.speed = s;
        self
    }

    pub fn axis(mut self, a: Vec3) -> Self {
        self.axis = a;
        self
    }

    /// Rotation quaternion at time `t` (identity before `start_delay`).
    pub fn rotation_at(&self, t: f32) -> Quat {
        if t < self.start_delay {
            Quat::IDENTITY
        } else {
            Quat::from_axis_angle(self.axis, self.speed * (t - self.start_delay))
        }
    }
}

// ---------------------------------------------------------------------------
// Slide
// ---------------------------------------------------------------------------

/// Translation that eases **from `base + delta` down to `base`** — the same
/// "starts offset, settles at rest" contract as [`Fall`] and [`Fade`], so a
/// text can slide in from off-frame and end up exactly where layout put it.
/// `stagger > 0` spreads the arrival across characters.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Slide {
    pub delta: Vec3,
    pub duration: f32,
    pub stagger: f32,
    pub ease: Ease,
    /// Absolute time (s) the slide begins; before it the text sits at
    /// `base + delta`, i.e. parked off-frame until its cue.
    pub start_at: f32,
}

impl Default for Slide {
    fn default() -> Self {
        Self {
            delta: Vec3::ZERO,
            duration: 2.0,
            stagger: 0.0,
            ease: Ease::Smooth,
            start_at: 0.0,
        }
    }
}

impl Slide {
    pub fn new(delta: impl Into<Vec3>) -> Self {
        Self {
            delta: delta.into(),
            ..Default::default()
        }
    }

    pub fn duration(mut self, d: f32) -> Self {
        self.duration = d;
        self
    }

    pub fn stagger(mut self, s: f32) -> Self {
        self.stagger = s;
        self
    }

    pub fn start_at(mut self, t: f32) -> Self {
        self.start_at = t;
        self
    }

    pub fn ease(mut self, e: Ease) -> Self {
        self.ease = e;
        self
    }
}

// ---------------------------------------------------------------------------
// Jitter
// ---------------------------------------------------------------------------

/// Continuous, deterministic hand-held wobble. Unlike [`Fall`] / [`Slide`] it
/// never settles: it is meant for text that is already on screen and should
/// not read as a frozen still.
///
/// `amplitude` is in world units at `size == 1` and is scaled by the text's
/// size, so the same numbers look right at any font size.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Jitter {
    /// Peak displacement per axis.
    pub amplitude: Vec3,
    /// Oscillations per second.
    pub frequency: f32,
    /// Peak rotational wobble, in radians (X/Y twist, Z roll).
    pub rotation: f32,
    /// Per-line phase so two simultaneous lines never move in lockstep.
    pub phase: f32,
    /// Absolute time (s) the wobble starts.
    pub start_at: f32,
    /// Per-character delay of the wobble start; `0` shakes the line as a unit.
    pub stagger: f32,
    /// Seconds spent ramping the amplitude in from zero, so a line parked at
    /// its cue does not snap sideways on the frame it appears.
    pub ramp: f32,
}

impl Default for Jitter {
    fn default() -> Self {
        Self {
            amplitude: Vec3::new(0.05, 0.04, 0.0),
            frequency: 5.0,
            rotation: 0.0,
            phase: 0.0,
            start_at: 0.0,
            stagger: 0.0,
            ramp: 0.3,
        }
    }
}

impl Jitter {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn amplitude(mut self, a: impl Into<Vec3>) -> Self {
        self.amplitude = a.into();
        self
    }

    pub fn frequency(mut self, f: f32) -> Self {
        self.frequency = f;
        self
    }

    pub fn rotation(mut self, r: f32) -> Self {
        self.rotation = r;
        self
    }

    pub fn start_at(mut self, t: f32) -> Self {
        self.start_at = t;
        self
    }

    pub fn stagger(mut self, s: f32) -> Self {
        self.stagger = s;
        self
    }
}

// ---------------------------------------------------------------------------
// Pop
// ---------------------------------------------------------------------------

/// Scale-in with overshoot — the classic 2D "bounce" entrance. Starts at
/// [`Pop::from`] (usually 0, i.e. zero size) and eases up to 1, optionally
/// flying past it before settling. `stagger` walks the pop across the line.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Pop {
    pub duration: f32,
    pub stagger: f32,
    /// Absolute time (s) the scale-in begins.
    pub start_at: f32,
    /// Scale at `u = 0`. `0` = starts at zero size, `1` = already at rest.
    pub from: f32,
    pub ease: Ease,
}

impl Default for Pop {
    fn default() -> Self {
        Self {
            duration: 0.45,
            stagger: 0.04,
            start_at: 0.0,
            from: 0.0,
            ease: Ease::BackOut,
        }
    }
}

impl Pop {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn duration(mut self, d: f32) -> Self {
        self.duration = d;
        self
    }

    pub fn stagger(mut self, s: f32) -> Self {
        self.stagger = s;
        self
    }

    pub fn start_at(mut self, t: f32) -> Self {
        self.start_at = t;
        self
    }

    pub fn from(mut self, s: f32) -> Self {
        self.from = s;
        self
    }

    pub fn ease(mut self, e: Ease) -> Self {
        self.ease = e;
        self
    }

    /// Scale factor at normalized time `u` in `[0, 1]`.
    pub fn scale_at(&self, u: f32) -> f32 {
        self.from + (1.0 - self.from) * self.ease.apply(u)
    }
}

// ---------------------------------------------------------------------------
// Wave
// ---------------------------------------------------------------------------

/// A sine wave travelling across the line: character `idx` sits `idx * spacing`
/// radians behind the head of the wave, so the whole line bounces in sequence —
/// the karaoke bounce every 2D lyric video leans on.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Wave {
    /// Vertical travel in world units at `size == 1`.
    pub amplitude: f32,
    /// Cycles per second.
    pub frequency: f32,
    /// Phase gap in radians between adjacent characters.
    pub spacing: f32,
    /// Absolute time (s) the wave starts.
    pub start_at: f32,
    /// Seconds spent ramping the amplitude in from zero.
    pub ramp: f32,
}

impl Default for Wave {
    fn default() -> Self {
        Self {
            amplitude: 0.22,
            frequency: 1.6,
            spacing: 0.55,
            start_at: 0.0,
            ramp: 0.4,
        }
    }
}

impl Wave {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn amplitude(mut self, a: f32) -> Self {
        self.amplitude = a;
        self
    }

    pub fn frequency(mut self, f: f32) -> Self {
        self.frequency = f;
        self
    }

    pub fn spacing(mut self, s: f32) -> Self {
        self.spacing = s;
        self
    }

    pub fn start_at(mut self, t: f32) -> Self {
        self.start_at = t;
        self
    }
}

// ---------------------------------------------------------------------------
// AnimationSpec
// ---------------------------------------------------------------------------

/// A single animation attached to a text (drives every character in it).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum AnimationSpec {
    Fall(Fall),
    Fade(Fade),
    Spin(Spin),
    Slide(Slide),
    Jitter(Jitter),
    Pop(Pop),
    Wave(Wave),
}

impl From<Fall> for AnimationSpec {
    fn from(v: Fall) -> Self {
        Self::Fall(v)
    }
}
impl From<Fade> for AnimationSpec {
    fn from(v: Fade) -> Self {
        Self::Fade(v)
    }
}
impl From<Spin> for AnimationSpec {
    fn from(v: Spin) -> Self {
        Self::Spin(v)
    }
}
impl From<Slide> for AnimationSpec {
    fn from(v: Slide) -> Self {
        Self::Slide(v)
    }
}
impl From<Jitter> for AnimationSpec {
    fn from(v: Jitter) -> Self {
        Self::Jitter(v)
    }
}
impl From<Pop> for AnimationSpec {
    fn from(v: Pop) -> Self {
        Self::Pop(v)
    }
}
impl From<Wave> for AnimationSpec {
    fn from(v: Wave) -> Self {
        Self::Wave(v)
    }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

/// Resolved transform of one character at a point in time.
///
/// `offset` is a **world-space** translation (already multiplied by the text's
/// `size`), `rotation` post-multiplies the text's own rotation, `scale` is a
/// **multiplicative** uniform factor (1.0 = untouched) and `alpha` multiplies
/// the text's own alpha.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AnimState {
    pub offset: Vec3,
    pub rotation: Quat,
    pub alpha: f32,
    pub scale: f32,
}

impl AnimState {
    pub const fn identity() -> Self {
        Self {
            offset: Vec3::ZERO,
            rotation: Quat::IDENTITY,
            alpha: 1.0,
            scale: 1.0,
        }
    }
}

impl Default for AnimState {
    fn default() -> Self {
        Self::identity()
    }
}

/// Evaluate a single animation for character `idx` at time `t`.
///
/// `size` is the text's world scale; all positional animation amplitudes are
/// multiplied by it so a `height: 4.0` fall looks the same on any font size.
pub fn eval_anim(anim: &AnimationSpec, t: f32, idx: usize, size: f32) -> AnimState {
    let mut st = AnimState::identity();
    match *anim {
        AnimationSpec::Fall(f) => {
            let delay = f.start_at + idx as f32 * f.stagger;
            let u = ((t - delay) / f.duration.max(1e-6)).clamp(0.0, 1.0);
            st.offset.y += f.offset_at(u) * size;
            if t > delay {
                let drift_t = (t - delay).min(f.duration);
                st.offset.x += f.drift * drift_t * size;
            }
        }
        AnimationSpec::Fade(f) => {
            let delay = f.start_at + idx as f32 * f.stagger;
            st.alpha = f.factor_at(t, delay);
        }
        AnimationSpec::Spin(s) => {
            st.rotation = s.rotation_at(t);
        }
        AnimationSpec::Slide(s) => {
            let delay = s.start_at + idx as f32 * s.stagger;
            let u = ((t - delay) / s.duration.max(1e-6)).clamp(0.0, 1.0);
            let k = (1.0 - s.ease.apply(u)) * size;
            st.offset += s.delta * k;
        }
        AnimationSpec::Jitter(j) => {
            let delay = j.start_at + idx as f32 * j.stagger;
            if t >= delay {
                let p = j.phase + idx as f32 * CHAR_PHASE;
                let n = jitter_noise(t, TAU * j.frequency.max(0.0), p);
                let env = smoothstep((t - delay) / j.ramp.max(1e-6));
                st.offset += Vec3::new(
                    n.x * j.amplitude.x,
                    n.y * j.amplitude.y,
                    n.z * j.amplitude.z,
                ) * (size * env);
                if j.rotation != 0.0 {
                    let r = j.rotation * env;
                    st.rotation = Quat::from_axis_angle(Vec3::Z, n.x * r)
                        * Quat::from_axis_angle(Vec3::Y, n.y * r * 0.5);
                }
            }
        }
        AnimationSpec::Pop(p) => {
            let delay = p.start_at + idx as f32 * p.stagger;
            let u = ((t - delay) / p.duration.max(1e-6)).clamp(0.0, 1.0);
            st.scale = p.scale_at(u);
        }
        AnimationSpec::Wave(w) => {
            if t >= w.start_at {
                let env = smoothstep((t - w.start_at) / w.ramp.max(1e-6));
                let phase = TAU * w.frequency * (t - w.start_at) - idx as f32 * w.spacing;
                st.offset.y += w.amplitude * env * phase.sin() * size;
            }
        }
    }
    st
}

/// Layer every animation of a text: offsets add, rotations multiply, scales
/// multiply, alphas multiply. This is the exact order the JS renderer must
/// reproduce.
pub fn eval_all(anims: &[AnimationSpec], t: f32, idx: usize, size: f32) -> AnimState {
    let mut out = AnimState::identity();
    for a in anims {
        let st = eval_anim(a, t, idx, size);
        out.offset += st.offset;
        out.rotation = out.rotation * st.rotation;
        out.alpha *= st.alpha;
        out.scale *= st.scale;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fall_starts_at_height_and_lands() {
        let f = Fall {
            duration: 2.4,
            stagger: 0.16,
            height: 9.0,
            gravity: -18.0,
            drift: 0.0,
            wobble: 0.0,
            start_at: 0.0,
        };
        assert!((f.offset_at(0.0) - 9.0).abs() < 1e-5);
        assert!(f.offset_at(1.0).abs() < 1e-4);
        assert!(f.offset_at(2.0) == 0.0, "must clamp past the landing");
        let s = AnimationSpec::Fall(f);
        assert!((eval_anim(&s, 0.0, 0, 2.0).offset.y - 18.0).abs() < 1e-4);
        assert!(eval_anim(&s, 5.0, 0, 2.0).offset.y.abs() < 1e-4);
        // stagger delays later characters
        let a = eval_anim(&s, 0.1, 1, 2.0).offset.y;
        assert!((a - 18.0).abs() < 1e-3, "char 1 must still be at the top");
    }

    #[test]
    fn slide_settles_at_base() {
        let s = AnimationSpec::Slide(Slide {
            delta: Vec3::new(0.0, -3.5, 0.0),
            duration: 1.6,
            stagger: 0.05,
            ease: Ease::EaseOutQuad,
            start_at: 0.0,
        });
        let start = eval_anim(&s, 0.0, 0, 1.5).offset;
        assert!((start.y - (-3.5 * 1.5)).abs() < 1e-4);
        let end = eval_anim(&s, 10.0, 0, 1.5).offset;
        assert!(end.y.abs() < 1e-4, "slide must end at the layout position");
        // stagger: char 1 is still fully displaced at t = 0
        let lag = eval_anim(&s, 0.04, 1, 1.5).offset;
        assert!((lag.y - (-3.5 * 1.5)).abs() < 1e-3);
    }

    #[test]
    fn fade_ramps_and_fades_out() {
        let f = Fade {
            duration: 1.0,
            stagger: 0.1,
            fade_out_at: Some(6.0),
            fade_out: 0.5,
            start_at: 0.0,
        };
        assert_eq!(f.factor_at(0.0, 0.0), 0.0);
        assert!((f.factor_at(0.5, 0.0) - 0.5).abs() < 1e-6);
        assert_eq!(f.factor_at(2.0, 0.0), 1.0);
        assert!((f.factor_at(6.25, 0.0) - 0.5).abs() < 1e-6);
        assert_eq!(f.factor_at(7.0, 0.0), 0.0);
    }

    #[test]
    fn spin_is_identity_before_delay() {
        let s = AnimationSpec::Spin(Spin {
            speed: 1.0,
            axis: Vec3::Y,
            start_delay: 2.0,
        });
        assert_eq!(eval_anim(&s, 1.0, 0, 1.0).rotation, Quat::IDENTITY);
        let r = eval_anim(&s, 3.0, 0, 1.0).rotation;
        assert_ne!(r, Quat::IDENTITY);
    }

    #[test]
    fn layered_animations_compose() {
        let anims = [
            AnimationSpec::Fall(Fall {
                duration: 1.0,
                stagger: 0.0,
                height: 4.0,
                gravity: -8.0,
                drift: 0.0,
                wobble: 0.0,
                start_at: 0.0,
            }),
            AnimationSpec::Fade(Fade {
                duration: 1.0,
                stagger: 0.0,
                fade_out_at: None,
                fade_out: 1.0,
                start_at: 0.0,
            }),
        ];
        let st = eval_all(&anims, 0.0, 0, 1.0);
        assert!((st.offset.y - 4.0).abs() < 1e-5);
        assert_eq!(st.alpha, 0.0, "fade gates the character");
        let done = eval_all(&anims, 5.0, 0, 1.0);
        assert!(done.offset.y.abs() < 1e-5);
        assert_eq!(done.alpha, 1.0);
    }

    #[test]
    fn start_at_delays_the_whole_cue() {
        let fade = AnimationSpec::Fade(Fade {
            duration: 1.0,
            stagger: 0.2,
            start_at: 5.0,
            fade_out_at: None,
            fade_out: 1.0,
        });
        assert_eq!(
            eval_anim(&fade, 4.9, 0, 1.0).alpha,
            0.0,
            "lyric line stays hidden before its cue"
        );
        assert!((eval_anim(&fade, 5.5, 0, 1.0).alpha - 0.5).abs() < 1e-6);
        assert!(
            (eval_anim(&fade, 5.5, 1, 1.0).alpha - 0.3).abs() < 1e-6,
            "char 1 lags by exactly one stagger"
        );
        assert_eq!(eval_anim(&fade, 7.0, 0, 1.0).alpha, 1.0);

        let slide = AnimationSpec::Slide(Slide {
            delta: Vec3::new(-4.0, 0.0, 0.0),
            duration: 1.0,
            stagger: 0.0,
            ease: Ease::Linear,
            start_at: 3.0,
        });
        assert!(
            (eval_anim(&slide, 2.0, 0, 1.0).offset.x - (-4.0)).abs() < 1e-4,
            "parked at base + delta until the cue"
        );
        assert!(eval_anim(&slide, 4.0, 0, 1.0).offset.x.abs() < 1e-4);

        let fall = AnimationSpec::Fall(Fall {
            duration: 1.0,
            stagger: 0.0,
            height: 5.0,
            gravity: -10.0,
            drift: 0.0,
            wobble: 0.0,
            start_at: 2.0,
        });
        assert!(
            (eval_anim(&fall, 1.0, 0, 1.0).offset.y - 5.0).abs() < 1e-4,
            "hangs at height until the cue"
        );
        assert!(eval_anim(&fall, 3.0, 0, 1.0).offset.y.abs() < 1e-4);
    }

    #[test]
    fn jitter_stays_parked_then_stays_within_its_amplitude() {
        let j = AnimationSpec::Jitter(Jitter {
            amplitude: Vec3::new(0.4, 0.3, 0.0),
            frequency: 5.0,
            rotation: 0.2,
            phase: 0.0,
            start_at: 4.0,
            stagger: 0.0,
            ramp: 0.5,
        });
        let parked = eval_anim(&j, 3.9, 0, 2.0);
        assert_eq!(parked.offset, Vec3::ZERO, "no wobble before the cue");
        assert_eq!(parked.rotation, Quat::IDENTITY);

        // Two half-sines per axis never exceed 1.0, so displacement stays
        // bounded by amplitude * size once the ramp has finished.
        for step in 0..400 {
            let t = 4.0 + step as f32 * 0.037;
            let st = eval_anim(&j, t, 0, 2.0);
            assert!(
                st.offset.x.abs() <= 0.4 * 2.0 + 1e-4,
                "x out of band at {t}"
            );
            assert!(
                st.offset.y.abs() <= 0.3 * 2.0 + 1e-4,
                "y out of band at {t}"
            );
            assert_eq!(st.scale, 1.0, "jitter must not resize the glyph");
        }

        // Neighbouring characters are decorrelated rather than moving in lock.
        let a = eval_anim(&j, 11.3, 0, 2.0).offset;
        let b = eval_anim(&j, 11.3, 1, 2.0).offset;
        assert!(a != b, "each character needs its own phase");
    }

    #[test]
    fn pop_scales_from_zero_and_bounces_past_one() {
        let p = AnimationSpec::Pop(Pop {
            duration: 0.5,
            stagger: 0.1,
            start_at: 2.0,
            from: 0.0,
            ease: Ease::BackOut,
        });
        assert!(
            eval_anim(&p, 1.0, 0, 1.0).scale.abs() < 1e-6,
            "parked at zero size"
        );
        assert_eq!(eval_anim(&p, 3.0, 0, 1.0).scale, 1.0, "settles at rest");
        let mid = eval_anim(&p, 2.15, 0, 1.0).scale;
        assert!(
            (0.0..1.0).contains(&mid),
            "mid-ramp must be growing, got {mid}"
        );

        // BackOut overshoots before it settles — that overshoot is the bounce.
        let mut overshot = false;
        for step in 0..100 {
            let t = 2.0 + step as f32 * 0.005;
            if eval_anim(&p, t, 0, 1.0).scale > 1.0 {
                overshot = true;
                break;
            }
        }
        assert!(overshot, "back_out must fly past 1.0");

        // Stagger: char 1 is still at zero size on the first char's start.
        assert!(eval_anim(&p, 2.0, 1, 1.0).scale.abs() < 1e-6);
    }

    #[test]
    fn wave_is_flat_before_start_and_bounces_per_character() {
        let w = AnimationSpec::Wave(Wave {
            amplitude: 0.5,
            frequency: 1.5,
            spacing: 0.7,
            start_at: 3.0,
            ramp: 0.4,
        });
        assert_eq!(
            eval_anim(&w, 2.9, 0, 1.0).offset.y,
            0.0,
            "flat before the cue"
        );
        assert_eq!(
            eval_anim(&w, 3.0, 0, 1.0).offset.y,
            0.0,
            "starts at zero displacement"
        );

        let mut moved = false;
        for step in 0..300 {
            let t = 3.4 + step as f32 * 0.021;
            let a = eval_anim(&w, t, 0, 2.0).offset.y;
            let b = eval_anim(&w, t, 1, 2.0).offset.y;
            assert!(a.abs() <= 0.5 * 2.0 + 1e-4, "amplitude bound at {t}");
            if a.abs() > 1e-3 {
                moved = true;
                assert!((a - b).abs() > 1e-6, "adjacent chars must differ");
            }
        }
        assert!(moved, "the wave has to actually move");
    }

    #[test]
    fn layered_state_multiplies_scale() {
        let anims = [
            AnimationSpec::Pop(Pop {
                duration: 1.0,
                stagger: 0.0,
                start_at: 0.0,
                from: 0.0,
                ease: Ease::Linear,
            }),
            AnimationSpec::Jitter(Jitter::default()),
        ];
        let st = eval_all(&anims, 0.5, 0, 1.0);
        assert!((st.scale - 0.5).abs() < 1e-6, "pop drives the scale");
        assert_eq!(eval_all(&anims, 10.0, 0, 1.0).scale, 1.0);
    }
}
