//! Declarative scene description.
//!
//! [`Scene`] is a plain data struct. Build it in Rust, or hand-write the JSON
//! form and load it with `serde_json`. [`Scene::export`] turns it into the
//! `scene_data.json` the JS/Three.js renderer consumes.

use std::path::{Path, PathBuf};
use std::time::Duration;

use serde::{Deserialize, Serialize};

use crate::animation::{AnimationSpec, Ease};
use crate::color::Color;
use crate::material::Material;
use crate::math::{Quat, Vec3};
use crate::text::FontList;

// ---------------------------------------------------------------------------
// serde helpers
// ---------------------------------------------------------------------------

mod duration_secs {
    use std::time::Duration;

    pub fn serialize<S: serde::Serializer>(d: &Duration, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_f32(d.as_secs_f32())
    }

    pub fn deserialize<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Duration, D::Error> {
        let secs = <f32 as serde::Deserialize>::deserialize(d)?;
        Ok(Duration::from_secs_f32(secs.max(0.0)))
    }
}

// ---------------------------------------------------------------------------
// Post-processing presets
// ---------------------------------------------------------------------------

/// Bloom preset. Physically-motivated defaults for neon text on a dark void.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct BloomConfig {
    pub enabled: bool,
    pub intensity: f32,
    pub threshold: f32,
    /// Higher = blurrier explosion.
    pub scale: f32,
    pub high_quality: bool,
}

impl BloomConfig {
    pub fn natural() -> Self {
        Self {
            enabled: true,
            intensity: 0.15,
            threshold: 0.9,
            scale: 2.0,
            high_quality: true,
        }
    }

    pub fn enabled(mut self, on: bool) -> Self {
        self.enabled = on;
        self
    }

    pub fn intensity(mut self, i: f32) -> Self {
        self.intensity = i;
        self
    }

    pub fn threshold(mut self, t: f32) -> Self {
        self.threshold = t;
        self
    }

    pub fn scale(mut self, s: f32) -> Self {
        self.scale = s;
        self
    }
}

impl Default for BloomConfig {
    fn default() -> Self {
        Self::natural()
    }
}

/// Depth-of-field preset.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct DofConfig {
    pub enabled: bool,
    pub focal_distance: f32,
    pub focal_radius: f32,
    /// In metres at 35 mm equivalent; scale of the Circle of Confusion.
    pub sensor_height: f32,
    pub aperture_f_stops: f32,
}

impl DofConfig {
    pub fn disabled() -> Self {
        Self {
            enabled: false,
            focal_distance: 10.0,
            focal_radius: 1.0,
            sensor_height: 0.035,
            aperture_f_stops: 4.0,
        }
    }

    pub fn enabled(mut self, on: bool) -> Self {
        self.enabled = on;
        self
    }

    pub fn focus(mut self, d: f32) -> Self {
        self.focal_distance = d;
        self
    }

    pub fn aperture(mut self, f_stops: f32) -> Self {
        self.aperture_f_stops = f_stops;
        self
    }
}

impl Default for DofConfig {
    fn default() -> Self {
        Self::disabled()
    }
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct CameraConfig {
    pub fov: f32,
    pub near: f32,
    pub far: f32,
    pub hdr: bool,
}

impl Default for CameraConfig {
    fn default() -> Self {
        Self {
            fov: 50.0_f32.to_radians(),
            near: 0.01,
            far: 500.0,
            hdr: true,
        }
    }
}

/// An eased camera move layered on top of the orbit: interpolate from the
/// orbit's own values at `at` toward whichever end values are set. Everything
/// is optional, so a move can be "just push in" or "push in while craning
/// down and drifting the look-at point".
///
/// The move is evaluated by the JS renderer (`js/scene.js::updateCamera`);
/// Rust only carries the description.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct CameraMove {
    /// Absolute time (s) the move starts.
    pub at: f32,
    pub duration: f32,
    pub ease: Ease,
    /// Orbit radius when the move finishes.
    pub radius_end: Option<f32>,
    /// Elevation (radians) when the move finishes.
    pub elevation_end: Option<f32>,
    /// Look-at point when the move finishes.
    pub target_end: Option<Vec3>,
}

impl Default for CameraMove {
    fn default() -> Self {
        Self {
            at: 0.0,
            duration: 6.0,
            ease: Ease::EaseInOutCubic,
            radius_end: None,
            elevation_end: None,
            target_end: None,
        }
    }
}

impl CameraMove {
    /// Push the camera in (or pull it out if `radius` is larger) over
    /// `duration` seconds starting at `at`.
    pub fn dolly(at: f32, duration: f32, radius_end: f32) -> Self {
        Self {
            at,
            duration,
            radius_end: Some(radius_end),
            ..Default::default()
        }
    }

    pub fn ease(mut self, e: Ease) -> Self {
        self.ease = e;
        self
    }

    pub fn elevation_end(mut self, e: f32) -> Self {
        self.elevation_end = Some(e);
        self
    }

    pub fn target_end(mut self, t: impl Into<Vec3>) -> Self {
        self.target_end = Some(t.into());
        self
    }
}

/// Hand-held shake applied to the whole rig: the camera and its look-at point
/// are translated together along the camera's own right/up axes, so the image
/// wanders on screen no matter which way the orbit is facing. A little roll on
/// top sells it as a human operator rather than a glitch.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct CameraShake {
    /// Peak screen-space displacement in world units.
    pub amplitude: f32,
    /// Oscillations per second.
    pub frequency: f32,
    /// Peak roll in radians.
    pub roll: f32,
    /// Absolute time (s) the shake starts.
    pub at: f32,
    /// Absolute time (s) it stops; `None` shakes for the whole scene.
    pub until: Option<f32>,
    /// Seconds spent fading in (and, when `until` is set, out).
    pub ramp: f32,
}

impl Default for CameraShake {
    fn default() -> Self {
        Self {
            amplitude: 0.07,
            frequency: 3.5,
            roll: 0.008,
            at: 0.0,
            until: None,
            ramp: 0.6,
        }
    }
}

impl CameraShake {
    pub fn new(amplitude: f32) -> Self {
        Self {
            amplitude,
            ..Default::default()
        }
    }

    pub fn frequency(mut self, f: f32) -> Self {
        self.frequency = f;
        self
    }

    pub fn roll(mut self, r: f32) -> Self {
        self.roll = r;
        self
    }

    pub fn between(mut self, at: f32, until: f32) -> Self {
        self.at = at;
        self.until = Some(until);
        self
    }

    /// Envelope at time `t`: fades in over `ramp`, holds, fades out over
    /// `ramp` before `until`. Zero outside `[at, until]`.
    pub fn envelope_at(&self, t: f32) -> f32 {
        if t < self.at {
            return 0.0;
        }
        if let Some(u) = self.until {
            if t > u {
                return 0.0;
            }
        }
        let ramp = self.ramp.max(1e-6);
        let mut env = ((t - self.at) / ramp).min(1.0);
        if let Some(u) = self.until {
            env = env.min(((u - t) / ramp).clamp(0.0, 1.0));
        }
        env.clamp(0.0, 1.0)
    }
}

/// Whole-scene camera animation (applied to the single camera).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct OrbitCam {
    /// Radius of the orbit circle around `target`, in world units.
    pub radius: f32,
    /// Fixed elevation angle above the XZ plane (radians).
    pub elevation: f32,
    /// Rotation speed in radians / second.
    pub speed: f32,
    /// Initial yaw (radians).
    pub start_yaw: f32,
    /// Point the camera keeps orbiting around / looking at.
    pub target: Vec3,
    /// Optional eased push / crane / pan layered on the orbit.
    pub movement: Option<CameraMove>,
    /// Optional hand-held shake.
    pub shake: Option<CameraShake>,
}

impl Default for OrbitCam {
    fn default() -> Self {
        Self {
            radius: 20.0,
            elevation: 0.25,
            speed: 0.4,
            start_yaw: 0.0,
            target: Vec3::ZERO,
            movement: None,
            shake: None,
        }
    }
}

impl OrbitCam {
    pub fn movement(mut self, m: CameraMove) -> Self {
        self.movement = Some(m);
        self
    }

    pub fn shake(mut self, s: CameraShake) -> Self {
        self.shake = Some(s);
        self
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum CameraAnim {
    Orbit(OrbitCam),
}

impl Default for CameraAnim {
    fn default() -> Self {
        Self::Orbit(OrbitCam::default())
    }
}

impl CameraAnim {
    pub fn orbit(radius: f32, speed: f32) -> Self {
        Self::Orbit(OrbitCam {
            radius,
            speed,
            ..Default::default()
        })
    }
}

// ---------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------

/// Key / rim / fill direct lights + ambient. All directional.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct LightSetup {
    pub key_color: Color,
    pub key_illuminance: f32,
    pub key_dir: Vec3,
    pub rim_color: Color,
    pub rim_illuminance: f32,
    pub rim_dir: Vec3,
    pub fill_color: Color,
    pub fill_illuminance: f32,
    pub fill_dir: Vec3,
    pub ambient_color: Color,
    pub ambient_brightness: f32,
}

impl Default for LightSetup {
    /// Multipass spotlight look on a dark stage.
    fn default() -> Self {
        Self {
            key_color: Color::srgb(1.0, 0.96, 0.9),
            key_illuminance: 3_000.0,
            key_dir: Vec3::new(0.6, 0.8, 0.5),
            rim_color: Color::srgb(0.35, 0.5, 1.0),
            rim_illuminance: 900.0,
            rim_dir: Vec3::new(-0.4, 0.1, -1.0),
            fill_color: Color::srgb(0.85, 0.9, 1.0),
            fill_illuminance: 500.0,
            fill_dir: Vec3::new(-0.7, 0.2, 0.6),
            ambient_color: Color::srgb(0.04, 0.04, 0.08),
            ambient_brightness: 40.0,
        }
    }
}

// ---------------------------------------------------------------------------
// Backgrounds (timeline) + post FX
// ---------------------------------------------------------------------------

/// One entry in the background timeline: a complete "look" the renderer
/// crossfades to at `at`. Entries are evaluated in list order, so a scene can
/// cut to a new palette mid-song without touching a single text spec.
///
/// Only the data lives here — the crossfade itself runs in `js/anim.mjs`
/// (`evalBackgrounds`), the same split the camera movement uses, so old scene
/// files deserialize unchanged and nothing has to mirror a second copy of the
/// arithmetic in Rust.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct BackgroundKey {
    /// Seconds from scene start at which this look takes over.
    pub at: f32,
    /// Crossfade length from the previous look, in seconds. `0` is a hard cut.
    pub fade: f32,
    /// Zenith colour (top of frame).
    pub top: Color,
    /// Nadir colour (bottom of frame).
    pub bottom: Color,
    /// Accent for the nebula / horizon glow.
    pub accent: Color,
    /// Volumetric cloud strength, 0..1.
    pub nebula: f32,
    /// Star density, 0..1.
    pub stars: f32,
    /// Background drift speed (parallax of cloud + stars).
    pub drift: f32,
}

impl Default for BackgroundKey {
    fn default() -> Self {
        Self {
            at: 0.0,
            fade: 1.5,
            top: Color::srgb(0.004, 0.008, 0.03),
            bottom: Color::srgb(0.02, 0.01, 0.05),
            accent: Color::srgb(0.1, 0.3, 0.9),
            nebula: 0.5,
            stars: 0.6,
            drift: 1.0,
        }
    }
}

impl BackgroundKey {
    /// Place a look at `at` seconds with the default palette.
    pub fn at(at: f32) -> Self {
        Self {
            at,
            ..Self::default()
        }
    }

    pub fn fade(mut self, seconds: f32) -> Self {
        self.fade = seconds;
        self
    }

    pub fn top(mut self, c: Color) -> Self {
        self.top = c;
        self
    }

    pub fn bottom(mut self, c: Color) -> Self {
        self.bottom = c;
        self
    }

    pub fn accent(mut self, c: Color) -> Self {
        self.accent = c;
        self
    }

    pub fn nebula(mut self, v: f32) -> Self {
        self.nebula = v.clamp(0.0, 1.0);
        self
    }

    pub fn stars(mut self, v: f32) -> Self {
        self.stars = v.clamp(0.0, 1.0);
        self
    }

    pub fn drift(mut self, v: f32) -> Self {
        self.drift = v;
        self
    }
}

/// Grade / distortion layered over the frame. Everything here is applied by a
/// single post pass in `js/scene.js` after tonemapping, so the numbers are
/// artistic rather than physical.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct FxConfig {
    pub enabled: bool,
    /// Base chromatic aberration, in screen units (0.0015 ≈ a visible fringe
    /// at the frame edge). Pulled on cue hits.
    pub chromatic: f32,
    /// Corner darkening, 0..1.
    pub vignette: f32,
    /// Film grain, 0..1.
    /// Extra saturation, 1.0 = untouched.
    pub saturation: f32,
    /// White flash mixed over the frame on every lyric cue.
    pub flash: f32,
    /// Camera push-in (in world units) fired on every lyric cue.
    pub punch: f32,
    /// How long a cue hit rings out, seconds.
    pub hit_duration: f32,
}

impl Default for FxConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            chromatic: 0.0,
            vignette: 0.0,
            saturation: 1.0,
            flash: 0.0,
            punch: 0.0,
            hit_duration: 0.35,
        }
    }
}

impl FxConfig {
    /// Turn on the whole rack with sensible starting values.
    pub fn on() -> Self {
        Self {
            enabled: true,
            chromatic: 0.0016,
            vignette: 0.55,
            saturation: 1.12,
            flash: 0.22,
            punch: 0.35,
            hit_duration: 0.35,
        }
    }
}

// ---------------------------------------------------------------------------
// One text line
// ---------------------------------------------------------------------------

/// A single line of text. Every character in the line gets its own mesh;
/// animations apply per character (with optional stagger).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct TextSpec {
    pub text: String,
    /// Font file; `None` -> walk [`Scene::font_fallback`] (or the OS font
    /// directory of the home platform).
    pub font: Option<String>,
    /// Font index inside a TTC/OTC collection (default 0).
    pub font_index: u32,
    /// World scale = glyph size in em units (1 em ~ font size 1 world unit).
    pub size: f32,
    /// Extrusion depth in em units.
    pub depth: f32,
    pub position: Vec3,
    pub rotation: Quat,
    pub scale: Vec3,
    pub material: Material,
    pub animations: Vec<AnimationSpec>,
    /// Global opacity multiplier for the line.
    pub alpha: f32,
    /// Optional world-space width budget. When the laid-out line is wider,
    /// `size` is scaled down so it fits. Font metrics only exist at export
    /// time, so this is the one place auto-fitting can happen — lyric lines
    /// run from three characters to twenty and would otherwise crop.
    pub fit_width: Option<f32>,
}

impl Default for TextSpec {
    fn default() -> Self {
        Self {
            text: String::new(),
            font: None,
            font_index: 0,
            size: 1.0,
            depth: 0.3,
            position: Vec3::ZERO,
            rotation: Quat::IDENTITY,
            scale: Vec3::ONE,
            material: Material::default(),
            animations: Vec::new(),
            alpha: 1.0,
            fit_width: None,
        }
    }
}

impl TextSpec {
    pub fn font(&mut self, path: impl Into<String>) -> &mut Self {
        self.font = Some(path.into());
        self
    }

    pub fn font_index(&mut self, idx: u32) -> &mut Self {
        self.font_index = idx;
        self
    }

    pub fn color(&mut self, c: impl Into<Color>) -> &mut Self {
        let c: Color = c.into();
        self.material = match self.material {
            Material::Neon(n) => Material::neon(c)
                .strength(n.strength)
                .outline_from(&Material::Neon(n)),
            Material::Metal(m) => Material::metal(c).roughness(m.roughness),
            Material::Glass(g) => Material::glass(c).roughness(g.roughness),
        };
        self
    }

    pub fn size(&mut self, s: f32) -> &mut Self {
        self.size = s;
        self
    }

    pub fn depth(&mut self, d: f32) -> &mut Self {
        self.depth = d;
        self
    }

    pub fn position(&mut self, p: impl Into<Vec3>) -> &mut Self {
        self.position = p.into();
        self
    }

    pub fn rotation(&mut self, r: Quat) -> &mut Self {
        self.rotation = r;
        self
    }

    pub fn material(&mut self, m: Material) -> &mut Self {
        self.material = m;
        self
    }

    /// Cap the laid-out line at this world-space width, shrinking `size` to
    /// match. `w <= 0` disables the budget.
    pub fn fit_width(&mut self, w: f32) -> &mut Self {
        self.fit_width = if w > 0.0 { Some(w) } else { None };
        self
    }

    /// Attach one animation. Call multiple times to layer them.
    pub fn animate(&mut self, anim: impl Into<AnimationSpec>) -> &mut Self {
        self.animations.push(anim.into());
        self
    }
}

// ---------------------------------------------------------------------------
// The scene
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Scene {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    #[serde(with = "duration_secs")]
    pub duration: Duration,
    pub background: Color,
    /// Ordered background looks. Empty = keep the flat `background` colour.
    pub backgrounds: Vec<BackgroundKey>,
    /// Post grade / distortion rack.
    pub fx: FxConfig,
    pub hdr: bool,
    pub bloom: BloomConfig,
    pub dof: DofConfig,
    pub fxaa: bool,
    pub camera: CameraConfig,
    pub camera_animation: Option<CameraAnim>,
    pub texts: Vec<TextSpec>,
    pub lights: LightSetup,
    /// Seed for any deterministic randomness (jitter etc.).
    pub seed: u64,
    /// Ordered fallback font files (by name) used when a `TextSpec.font` is
    /// missing or a glyph is absent.
    pub font_fallback: Vec<String>,
    /// Optional explicit ffmpeg executable path.
    pub ffmpeg: Option<PathBuf>,
    /// Directory for intermediate files.
    pub work_dir: PathBuf,
}

impl Default for Scene {
    fn default() -> Self {
        Scene::new(1920, 1080, 60)
    }
}

impl Scene {
    pub fn new(width: u32, height: u32, fps: u32) -> Self {
        Self {
            width,
            height,
            fps,
            duration: Duration::from_secs(5),
            background: Color::srgb(0.003, 0.004, 0.012),
            backgrounds: Vec::new(),
            fx: FxConfig::default(),
            hdr: true,
            bloom: BloomConfig::natural(),
            dof: DofConfig::disabled(),
            fxaa: false,
            camera: CameraConfig::default(),
            camera_animation: Some(CameraAnim::default()),
            texts: Vec::new(),
            lights: LightSetup::default(),
            seed: 0xC0FFEE,
            font_fallback: FontList::os_default_fonts(),
            ffmpeg: None,
            work_dir: PathBuf::from("target/chamina"),
        }
    }

    // -- chainable setters -----------------------------------------------

    pub fn duration(&mut self, d: Duration) -> &mut Self {
        self.duration = d;
        self
    }

    pub fn duration_secs(&mut self, secs: f32) -> &mut Self {
        self.duration = Duration::from_secs_f32(secs.max(0.0));
        self
    }

    pub fn fps(&mut self, f: u32) -> &mut Self {
        self.fps = f;
        self
    }

    pub fn background(&mut self, c: impl Into<Color>) -> &mut Self {
        self.background = c.into();
        self
    }

    /// Append one look to the background timeline. Call repeatedly to cut
    /// between palettes during the piece.
    pub fn background_key(&mut self, key: BackgroundKey) -> &mut Self {
        self.backgrounds.push(key);
        self
    }

    pub fn fx(&mut self, f: FxConfig) -> &mut Self {
        self.fx = f;
        self
    }

    pub fn hdr(&mut self, on: bool) -> &mut Self {
        self.hdr = on;
        self
    }

    pub fn bloom(&mut self, c: BloomConfig) -> &mut Self {
        self.bloom = c;
        self
    }

    pub fn dof(&mut self, d: DofConfig) -> &mut Self {
        self.dof = d;
        self
    }

    pub fn fxaa(&mut self, on: bool) -> &mut Self {
        self.fxaa = on;
        self
    }

    pub fn orbit(&mut self, cam: OrbitCam) -> &mut Self {
        self.camera_animation = Some(CameraAnim::Orbit(cam));
        self
    }

    pub fn lights(&mut self, l: LightSetup) -> &mut Self {
        self.lights = l;
        self
    }

    pub fn seed(&mut self, s: u64) -> &mut Self {
        self.seed = s;
        self
    }

    pub fn fallback_fonts(&mut self, paths: &[impl AsRef<str> + Clone]) -> &mut Self {
        self.font_fallback = paths.iter().map(|p| p.as_ref().to_string()).collect();
        self
    }

    pub fn ffmpeg(&mut self, p: impl Into<PathBuf>) -> &mut Self {
        self.ffmpeg = Some(p.into());
        self
    }

    pub fn work_dir(&mut self, d: impl Into<PathBuf>) -> &mut Self {
        self.work_dir = d.into();
        self
    }

    // -- texts -----------------------------------------------------------

    /// Append a new text line and return a mutable reference to it, so you can
    /// chain `.font(..).size(..).animate(..)` and so on.
    pub fn add_text(&mut self, text: impl Into<String>) -> &mut TextSpec {
        self.texts.push(TextSpec {
            text: text.into(),
            ..Default::default()
        });
        self.texts.last_mut().unwrap()
    }

    // -- export ----------------------------------------------------------

    /// Compute every glyph mesh and write `scene_data.json` for the renderer.
    pub fn export(&self, out: impl AsRef<Path>) -> crate::Result<()> {
        crate::export::export(self, out.as_ref())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn old_camera_json_still_parses_without_movement_or_shake() {
        // What scenes written before the camera effects existed look like.
        let legacy = r#"{"type":"orbit","radius":14.0,"elevation":0.12,"speed":0.0,
                         "start_yaw":1.5707964,"target":[0.0,0.0,0.0]}"#;
        let cam: CameraAnim = serde_json::from_str(legacy).unwrap();
        match cam {
            CameraAnim::Orbit(o) => {
                assert_eq!(o.radius, 14.0);
                assert!(o.movement.is_none(), "missing key must default to None");
                assert!(o.shake.is_none(), "missing key must default to None");
            }
        }
    }

    #[test]
    fn camera_move_and_shake_round_trip() {
        let cam = CameraAnim::Orbit(
            OrbitCam::default()
                .movement(CameraMove::dolly(2.0, 8.0, 9.5).elevation_end(0.05))
                .shake(CameraShake::new(0.12).between(1.0, 4.0)),
        );
        let json = serde_json::to_string(&cam).unwrap();
        let back: CameraAnim = serde_json::from_str(&json).unwrap();
        match back {
            CameraAnim::Orbit(o) => {
                let m = o.movement.expect("movement survives the trip");
                assert_eq!(m.at, 2.0);
                assert_eq!(m.duration, 8.0);
                assert_eq!(m.radius_end, Some(9.5));
                assert_eq!(m.elevation_end, Some(0.05));
                assert!(m.target_end.is_none());
                let s = o.shake.expect("shake survives the trip");
                assert_eq!(s.amplitude, 0.12);
                assert_eq!(s.until, Some(4.0));
            }
        }
    }

    #[test]
    fn shake_envelope_fades_in_holds_and_fades_out() {
        let s = CameraShake::new(0.1).between(2.0, 5.0);
        assert_eq!(s.envelope_at(1.0), 0.0, "before the start");
        assert_eq!(s.envelope_at(6.0), 0.0, "after the end");
        assert_eq!(s.envelope_at(2.0), 0.0, "ramp begins from zero");
        assert_eq!(s.envelope_at(3.5), 1.0, "fully open in the middle");
        assert_eq!(s.envelope_at(5.0), 0.0, "ramp ends at zero");
        let half = s.envelope_at(4.7);
        assert!((half - 0.5).abs() < 1e-6, "symmetric fade-out, got {half}");

        let forever = CameraShake::new(0.1);
        assert_eq!(forever.envelope_at(0.0), 0.0);
        assert_eq!(forever.envelope_at(1.0), 1.0);
        assert_eq!(
            forever.envelope_at(1e6),
            1.0,
            "no `until` means no fade-out"
        );
    }

    #[test]
    fn scene_json_written_before_backgrounds_still_parses() {
        let mut v = serde_json::to_value(Scene::new(1920, 1080, 60)).unwrap();
        v.as_object_mut().unwrap().remove("backgrounds");
        v.as_object_mut().unwrap().remove("fx");
        let scene: Scene = serde_json::from_value(v).unwrap();
        assert!(
            scene.backgrounds.is_empty(),
            "absent timeline = flat colour"
        );
        assert!(!scene.fx.enabled, "absent rack stays off");
        assert_eq!(scene.fx.saturation, 1.0, "no grade by default");
    }

    #[test]
    fn a_background_key_round_trips_with_its_palette() {
        let key = BackgroundKey::at(42.0)
            .fade(0.0)
            .top(Color::srgb(0.0, 0.1, 0.3))
            .accent(Color::srgb(1.0, 0.5, 0.0))
            .nebula(0.8)
            .stars(0.25)
            .drift(2.5);
        let json = serde_json::to_string(&key).unwrap();
        let back: BackgroundKey = serde_json::from_str(&json).unwrap();
        // `Color` crosses the bridge as `#rrggbb`, so an arbitrary float comes
        // back quantised to 8 bits — compare the way the renderer sees it.
        assert_eq!(back.top.to_hex(), key.top.to_hex());
        assert_eq!(back.bottom.to_hex(), key.bottom.to_hex());
        assert_eq!(back.accent.to_hex(), key.accent.to_hex());
        assert_eq!(back.at, 42.0);
        assert_eq!(back.fade, 0.0, "0 must survive as a hard cut");
        assert_eq!(back.nebula, 0.8);
        assert_eq!(back.stars, 0.25);
        assert_eq!(back.drift, 2.5);
    }

    #[test]
    fn clamped_slider_values_cannot_escape_0_to_1() {
        let key = BackgroundKey::default().nebula(7.0).stars(-3.0);
        assert_eq!(key.nebula, 1.0);
        assert_eq!(key.stars, 0.0);
    }

    #[test]
    fn the_fx_rack_starts_gated_and_flips_on_with_a_full_set_of_values() {
        let off = FxConfig::default();
        assert!(
            !off.enabled,
            "scenes that never mention fx render untouched"
        );
        assert_eq!(off.flash, 0.0);

        let on = FxConfig::on();
        assert!(on.enabled);
        assert!(
            on.chromatic > 0.0 && on.chromatic < 0.01,
            "fringe, not smear"
        );
        assert!(on.saturation > 1.0);
        assert!(on.vignette > 0.0 && on.vignette <= 1.0);
    }
}
