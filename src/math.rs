//! Minimal 3D math types: `Vec2`, `Vec3`, `Quat`.
//!
//! A tiny, dependency-free subset of the vector/quaternion math chamina needs.
//! Both types serialize as plain JSON arrays (`[x, y, z]` / `[x, y, z, w]`).

// ---------------------------------------------------------------------------
// Vec2
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Vec2 {
    pub x: f32,
    pub y: f32,
}

impl Vec2 {
    pub const ZERO: Vec2 = Vec2 { x: 0.0, y: 0.0 };

    pub const fn new(x: f32, y: f32) -> Self {
        Vec2 { x, y }
    }

    pub fn to_array(self) -> [f32; 2] {
        [self.x, self.y]
    }
}

impl serde::Serialize for Vec2 {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        (self.x, self.y).serialize(s)
    }
}

// ---------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vec3 {
    pub const ZERO: Vec3 = Vec3 {
        x: 0.0,
        y: 0.0,
        z: 0.0,
    };
    pub const ONE: Vec3 = Vec3 {
        x: 1.0,
        y: 1.0,
        z: 1.0,
    };
    pub const X: Vec3 = Vec3 {
        x: 1.0,
        y: 0.0,
        z: 0.0,
    };
    pub const Y: Vec3 = Vec3 {
        x: 0.0,
        y: 1.0,
        z: 0.0,
    };
    pub const Z: Vec3 = Vec3 {
        x: 0.0,
        y: 0.0,
        z: 1.0,
    };

    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Vec3 { x, y, z }
    }

    pub const fn splat(v: f32) -> Self {
        Vec3::new(v, v, v)
    }

    pub fn dot(self, o: Vec3) -> f32 {
        self.x * o.x + self.y * o.y + self.z * o.z
    }

    pub fn length(self) -> f32 {
        self.dot(self).sqrt()
    }

    pub fn normalize(self) -> Vec3 {
        let len = self.length();
        if len > 1e-6 {
            self * (1.0 / len)
        } else {
            Vec3::ZERO
        }
    }

    pub fn cross(self, o: Vec3) -> Vec3 {
        Vec3::new(
            self.y * o.z - self.z * o.y,
            self.z * o.x - self.x * o.z,
            self.x * o.y - self.y * o.x,
        )
    }

    pub fn to_array(self) -> [f32; 3] {
        [self.x, self.y, self.z]
    }
}

impl std::ops::Add for Vec3 {
    type Output = Vec3;
    fn add(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x + o.x, self.y + o.y, self.z + o.z)
    }
}

impl std::ops::Sub for Vec3 {
    type Output = Vec3;
    fn sub(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x - o.x, self.y - o.y, self.z - o.z)
    }
}

impl std::ops::Mul<f32> for Vec3 {
    type Output = Vec3;
    fn mul(self, s: f32) -> Vec3 {
        Vec3::new(self.x * s, self.y * s, self.z * s)
    }
}

impl std::ops::AddAssign for Vec3 {
    fn add_assign(&mut self, o: Vec3) {
        self.x += o.x;
        self.y += o.y;
        self.z += o.z;
    }
}

impl From<[f32; 3]> for Vec3 {
    fn from(a: [f32; 3]) -> Self {
        Vec3::new(a[0], a[1], a[2])
    }
}

impl From<(f32, f32, f32)> for Vec3 {
    fn from(t: (f32, f32, f32)) -> Self {
        Vec3::new(t.0, t.1, t.2)
    }
}

impl serde::Serialize for Vec3 {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        (self.x, self.y, self.z).serialize(s)
    }
}

impl<'de> serde::Deserialize<'de> for Vec3 {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let (x, y, z) = <(f32, f32, f32)>::deserialize(d)?;
        Ok(Vec3::new(x, y, z))
    }
}

// ---------------------------------------------------------------------------
// Quat (unit quaternion, Hamilton product)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Quat {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

impl Quat {
    pub const IDENTITY: Quat = Quat {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 1.0,
    };

    /// Rotation of `angle` radians around `axis` (axis is normalized here).
    pub fn from_axis_angle(axis: Vec3, angle: f32) -> Quat {
        let axis = axis.normalize();
        let half = angle * 0.5;
        let s = half.sin();
        Quat {
            x: axis.x * s,
            y: axis.y * s,
            z: axis.z * s,
            w: half.cos(),
        }
    }

    /// Rotate a vector by this quaternion.
    pub fn mul_vec3(self, v: Vec3) -> Vec3 {
        let qv = Vec3::new(self.x, self.y, self.z);
        let t = qv.cross(v) * 2.0;
        v + t * self.w + qv.cross(t)
    }

    pub fn to_array(self) -> [f32; 4] {
        [self.x, self.y, self.z, self.w]
    }
}

impl Default for Quat {
    fn default() -> Self {
        Quat::IDENTITY
    }
}

/// Hamilton product: `(a * b)` applies `b` first, then `a`.
impl std::ops::Mul for Quat {
    type Output = Quat;
    fn mul(self, b: Quat) -> Quat {
        Quat {
            x: self.w * b.x + self.x * b.w + self.y * b.z - self.z * b.y,
            y: self.w * b.y - self.x * b.z + self.y * b.w + self.z * b.x,
            z: self.w * b.z + self.x * b.y - self.y * b.x + self.z * b.w,
            w: self.w * b.w - self.x * b.x - self.y * b.y - self.z * b.z,
        }
    }
}

impl serde::Serialize for Quat {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        (self.x, self.y, self.z, self.w).serialize(s)
    }
}

impl<'de> serde::Deserialize<'de> for Quat {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let (x, y, z, w) = <(f32, f32, f32, f32)>::deserialize(d)?;
        Ok(Quat { x, y, z, w })
    }
}
