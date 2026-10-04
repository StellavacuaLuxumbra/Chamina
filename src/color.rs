//! Minimal sRGB color type.
//!
//! Serialized as a `#rrggbb` hex string so JS can hand it straight to
//! `new THREE.Color('#rrggbb')`.

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Color {
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

impl Color {
    pub const CYAN: Color = Color {
        r: 0.0,
        g: 1.0,
        b: 1.0,
        a: 1.0,
    };
    pub const WHITE: Color = Color {
        r: 1.0,
        g: 1.0,
        b: 1.0,
        a: 1.0,
    };
    pub const BLACK: Color = Color {
        r: 0.0,
        g: 0.0,
        b: 0.0,
        a: 1.0,
    };

    pub fn srgb(r: f32, g: f32, b: f32) -> Self {
        Color { r, g, b, a: 1.0 }
    }

    pub fn srgba(r: f32, g: f32, b: f32, a: f32) -> Self {
        Color { r, g, b, a }
    }

    fn byte(v: f32) -> u8 {
        (v.clamp(0.0, 1.0) * 255.0 + 0.5) as u8
    }

    /// `#rrggbb` (alpha is dropped).
    pub fn to_hex(self) -> String {
        format!(
            "#{:02x}{:02x}{:02x}",
            Self::byte(self.r),
            Self::byte(self.g),
            Self::byte(self.b)
        )
    }

    /// `#rrggbbaa`.
    pub fn to_hex_alpha(self) -> String {
        format!(
            "#{:02x}{:02x}{:02x}{:02x}",
            Self::byte(self.r),
            Self::byte(self.g),
            Self::byte(self.b),
            Self::byte(self.a)
        )
    }
}

impl Default for Color {
    fn default() -> Self {
        Color::WHITE
    }
}

impl From<&str> for Color {
    fn from(s: &str) -> Self {
        let s = s.trim();
        let s = s.strip_prefix('#').unwrap_or(s);
        let Ok(v) = u32::from_str_radix(s, 16) else {
            return Color::WHITE;
        };
        match s.len() {
            8 => Color {
                r: ((v >> 24) & 0xff) as f32 / 255.0,
                g: ((v >> 16) & 0xff) as f32 / 255.0,
                b: ((v >> 8) & 0xff) as f32 / 255.0,
                a: (v & 0xff) as f32 / 255.0,
            },
            6 => Color {
                r: ((v >> 16) & 0xff) as f32 / 255.0,
                g: ((v >> 8) & 0xff) as f32 / 255.0,
                b: (v & 0xff) as f32 / 255.0,
                a: 1.0,
            },
            3 => Color {
                r: ((v >> 8) & 0xf) as f32 / 15.0,
                g: ((v >> 4) & 0xf) as f32 / 15.0,
                b: (v & 0xf) as f32 / 15.0,
                a: 1.0,
            },
            _ => Color::WHITE,
        }
    }
}

impl From<String> for Color {
    fn from(s: String) -> Self {
        Color::from(s.as_str())
    }
}

impl From<[f32; 3]> for Color {
    fn from(a: [f32; 3]) -> Self {
        Color::srgb(a[0], a[1], a[2])
    }
}

impl From<(f32, f32, f32)> for Color {
    fn from(t: (f32, f32, f32)) -> Self {
        Color::srgb(t.0, t.1, t.2)
    }
}

impl serde::Serialize for Color {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&self.to_hex())
    }
}

impl<'de> serde::Deserialize<'de> for Color {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = <String as serde::Deserialize>::deserialize(d)?;
        Ok(Color::from(s.as_str()))
    }
}
