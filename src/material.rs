//! Material presets: neon glow, metal, glass.
//!
//! The renderer (JS/Three.js) reads these tagged unions directly — there is no
//! engine-specific material conversion on the Rust side any more.

use serde::{Deserialize, Serialize};

use crate::color::Color;

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Neon {
    pub color: Color,
    /// Emissive multiplier (>1 pushes HDR luminance for Bloom).
    pub strength: f32,
    /// Inverted-hull rim drawn around the glyph silhouette, if any.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub outline: Option<Outline>,
}

/// A contrasting rim traced around a glyph by inflating its hull along the
/// vertex normals (`width` is in em, so it scales with the text size).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Outline {
    pub color: Color,
    pub width: f32,
    /// Rim emissive multiplier. 1.0 keeps the rim crisp and outside Bloom's
    /// threshold; >1 lets the contour glow like the glyph body.
    #[serde(default = "outline_strength_default")]
    pub strength: f32,
}

fn outline_strength_default() -> f32 {
    1.0
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Metal {
    pub color: Color,
    pub roughness: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Glass {
    pub color: Color,
    pub roughness: f32,
    /// Base opacity of the glass body.
    pub opacity: f32,
}

/// The final material style a text uses.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Material {
    Neon(Neon),
    Metal(Metal),
    Glass(Glass),
}

impl Default for Material {
    fn default() -> Self {
        Self::neon(Color::CYAN)
    }
}

impl Material {
    pub fn neon(color: impl Into<Color>) -> Self {
        Self::Neon(Neon {
            color: color.into(),
            strength: 5.0,
            outline: None,
        })
    }

    pub fn metal(color: impl Into<Color>) -> Self {
        Self::Metal(Metal {
            color: color.into(),
            roughness: 0.3,
        })
    }

    pub fn glass(color: impl Into<Color>) -> Self {
        Self::Glass(Glass {
            color: color.into(),
            roughness: 0.05,
            opacity: 0.25,
        })
    }

    /// Builder tweaks ------------------------------------------------------
    pub fn strength(mut self, s: f32) -> Self {
        if let Self::Neon(n) = &mut self {
            n.strength = s;
        }
        self
    }

    /// Trace a rim of `color` around the glyph silhouette, `width` em wide.
    pub fn outline(mut self, color: impl Into<Color>, width: f32) -> Self {
        if let Self::Neon(n) = &mut self {
            n.outline = Some(Outline {
                color: color.into(),
                width,
                strength: outline_strength_default(),
            });
        }
        self
    }

    /// Carry the outline over when a material is rebuilt (color changes).
    pub fn outline_from(mut self, other: &Material) -> Self {
        if let (Self::Neon(a), Material::Neon(b)) = (&mut self, other) {
            a.outline = b.outline;
        }
        self
    }

    pub fn roughness(mut self, r: f32) -> Self {
        match &mut self {
            Self::Metal(m) => m.roughness = r,
            Self::Glass(g) => g.roughness = r,
            Self::Neon(_) => {}
        }
        self
    }

    /// Flat, unlit base colour of the material (used for outlines / UI chips).
    pub fn base_color(&self) -> Color {
        match *self {
            Material::Neon(n) => n.color,
            Material::Metal(m) => m.color,
            Material::Glass(g) => g.color,
        }
    }

    /// Emissive colour multiplied by `strength` — what Bloom sees for neon.
    pub fn emissive(&self) -> Option<(Color, f32)> {
        match *self {
            Material::Neon(n) => Some((n.color, n.strength)),
            _ => None,
        }
    }
}
