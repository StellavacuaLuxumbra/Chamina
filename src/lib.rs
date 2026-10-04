//! # chamina
//!
//! A code-driven 3D text PV library. Declare a scene — text, fonts,
//! animations, camera and post effects — and chamina computes the glyph
//! meshes and animation curves, then exports a JSON scene that a JS/Three.js
//! renderer turns into a video.
//!
//! ```no_run
//! use chamina::prelude::*;
//! use std::time::Duration;
//!
//! let mut scene = Scene::new(1920, 1080, 60);
//! scene.duration(Duration::from_secs(8));
//! scene
//!     .add_text("你好，世界 Hello 世界")
//!     .font("fonts/NotoSansCJK.ttf")
//!     .size(1.0)
//!     .depth(0.3)
//!     .position([0.0, 10.0, 0.0])
//!     .animate(Fall::new().duration(3.0).stagger(0.1))
//!     .material(Material::neon(Color::CYAN));
//! scene.export("out/scene_data.json")?;
//! # Ok::<(), chamina::ChaminaError>(())
//! ```

#![forbid(unsafe_code)]

mod error;
pub use error::{ChaminaError, Result};

pub mod animation;
pub mod color;
pub mod export;
pub mod material;
pub mod math;
pub mod scene;
pub mod text;

/// Re-exports the crate plus common convenience types.
pub mod prelude {
    pub use crate::animation::{AnimationSpec, Ease, Fade, Fall, Jitter, Pop, Slide, Spin, Wave};
    pub use crate::color::Color;
    pub use crate::material::{Glass, Material, Metal, Neon, Outline};
    pub use crate::math::{Quat, Vec2, Vec3};
    pub use crate::scene::{
        BackgroundKey, BloomConfig, CameraAnim, CameraConfig, CameraMove, CameraShake, DofConfig,
        FxConfig, LightSetup, OrbitCam, Scene, TextSpec,
    };
    pub use crate::ChaminaError;
    pub use crate::Result;
}
