//! Font loading and glyph access.

use std::collections::HashSet;
use std::path::PathBuf;

use crate::error::{ChaminaError, Result};

/// A font file kept in memory, ready to be parsed on demand.
///
/// `ttf-parser` borrows the byte buffer, so we keep the raw bytes alive and
/// parse a cheap `Face` each time we need it.
#[derive(Debug, Clone)]
pub struct OwnedFont {
    pub path: PathBuf,
    pub index: u32,
    data: Vec<u8>,
}

impl OwnedFont {
    pub fn from_file(path: impl Into<PathBuf>, index: u32) -> Result<Self> {
        let path = path.into();
        let data = std::fs::read(&path).map_err(|_| ChaminaError::FontRead(path.clone()))?;
        Self::from_bytes(path, index, data)
    }

    fn from_bytes(path: PathBuf, index: u32, data: Vec<u8>) -> Result<Self> {
        // Validate eagerly so errors surface near the source.
        let _ =
            Self::parse_face(&data, index).map_err(|_| ChaminaError::FontFormat(path.clone()))?;
        Ok(Self { path, index, data })
    }

    fn parse_face(
        data: &[u8],
        index: u32,
    ) -> std::result::Result<ttf_parser::Face<'_>, ttf_parser::FaceParsingError> {
        ttf_parser::Face::parse(data, index)
    }

    /// Parse the face (cheap) for a single use.
    pub fn face(&self) -> Result<ttf_parser::Face<'_>> {
        Self::parse_face(&self.data, self.index)
            .map_err(|_| ChaminaError::FontFormat(self.path.clone()))
    }
}

/// A font requirement: file + collection index.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FontReq {
    pub path: String,
    pub index: u32,
}

impl FontReq {
    pub fn file(path: impl Into<String>) -> Self {
        Self {
            path: path.into(),
            index: 0,
        }
    }
}

/// Ordered list of loaded fonts. When one font lacks a glyph, the next is
/// tried (CJK fallback chain).
#[derive(Debug, Clone, Default)]
pub struct FontList {
    pub fonts: Vec<OwnedFont>,
}

impl FontList {
    /// Load a chain of fonts, first matching path wins for duplicates.
    /// Missing files are skipped silently (they were only fallbacks).
    pub fn load(reqs: &[FontReq]) -> Result<Self> {
        let mut fonts = Vec::with_capacity(reqs.len());
        let mut seen = HashSet::with_capacity(reqs.len());
        for req in reqs {
            let key = (req.path.clone(), req.index);
            if !seen.insert(key) {
                continue;
            }
            match OwnedFont::from_file(&req.path, req.index) {
                Ok(f) => fonts.push(f),
                Err(_) => continue, // fallback candidate absent
            }
        }
        Ok(Self { fonts })
    }

    pub fn is_empty(&self) -> bool {
        self.fonts.is_empty()
    }

    /// Built-in candidate list for the host OS (executable machine).
    pub fn os_default_fonts() -> Vec<String> {
        let mut v = Vec::new();
        #[cfg(target_os = "windows")]
        for p in [
            r"C:\Windows\Fonts\msyh.ttc", // 微软雅黑 (CJK + Latin)
            r"C:\Windows\Fonts\msyh.ttf",
            r"C:\Windows\Fonts\segoeui.ttf",
            r"C:\Windows\Fonts\arial.ttf",
        ] {
            if std::path::Path::new(p).exists() {
                v.push(p.to_string());
            }
        }
        #[cfg(target_os = "macos")]
        for p in [
            "/System/Library/Fonts/PingFang.ttc",
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
            "/System/Library/Fonts/Helvetica.dfont",
        ] {
            if std::path::Path::new(p).exists() {
                v.push(p.to_string());
            }
        }
        #[cfg(target_os = "linux")]
        for p in [
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        ] {
            if std::path::Path::new(p).exists() {
                v.push(p.to_string());
            }
        }
        v
    }
}
