//! Text line layout: per-character advance, fallback and centering.

use crate::error::{ChaminaError, Result};
use crate::math::Vec2;
use crate::text::font::FontList;

/// Per-character placement (in em units, x origin = line center).
#[derive(Debug, Clone, Copy)]
pub struct GlyphItem {
    pub ch: char,
    /// Index into the [`FontList`] that supplied the glyph.
    pub font_idx: usize,
    pub glyph_id: u16,
    /// Glyph center in em, relative to the line origin (which is centered on X).
    pub center: Vec2,
    /// Horizontal advance of the glyph from the *same* font (em).
    pub advance_em: f32,
    /// True when the glyph has a real outline (draws a mesh).
    pub has_outline: bool,
}

/// The result of laying out one line of text.
#[derive(Debug, Clone)]
pub struct TextLayout {
    pub glyphs: Vec<GlyphItem>,
    /// Total advance width of the line in em.
    pub total_width: f32,
    /// Em-corrected vertical center of the longest font used.
    pub line_center_y: f32,
}

/// Layout one line: choose per-character fonts via fallback chain, compute
/// advances in em, center the line horizontally and optically vertically.
pub fn layout_line(chars: &str, fonts: &FontList) -> Result<TextLayout> {
    if fonts.is_empty() {
        return Err(ChaminaError::NoFont);
    }

    let mut glyphs = Vec::with_capacity(chars.chars().count());
    let mut cursor = 0.0_f32;

    // Track ascender/descender across the fonts actually used so mixed CJK +
    // Latin lines sit on a common baseline.
    let mut max_asc = -f32::MAX;
    let mut min_desc = f32::MAX;

    for ch in chars.chars() {
        let (font_idx, glyph_id, advance_em, has_outline) = pick_glyph(ch, fonts)?;
        let upem = {
            let face = fonts.fonts[font_idx].face()?;
            face.units_per_em() as f32
        };
        let asc = fonts.fonts[font_idx].face()?.ascender() as f32 / upem;
        let desc = fonts.fonts[font_idx].face()?.descender() as f32 / upem;
        max_asc = max_asc.max(asc);
        min_desc = min_desc.min(desc);

        let center = Vec2::new(cursor + advance_em * 0.5, 0.0);
        glyphs.push(GlyphItem {
            ch,
            font_idx,
            glyph_id,
            center,
            advance_em,
            has_outline,
        });
        cursor += advance_em;
    }

    let total_width = cursor;
    let line_center_y = (max_asc + min_desc) * 0.5;

    let mut layout = TextLayout {
        glyphs,
        total_width,
        line_center_y,
    };
    // Center relative to the origin (apply as entity-space offset).
    let half = total_width * 0.5;
    for g in &mut layout.glyphs {
        g.center.x -= half;
        g.center.y -= line_center_y;
    }
    Ok(layout)
}

/// Find the first font in the chain that can render `ch` with a real outline.
/// Falls back to a font with a (non-zero) advance but no outline (e.g. space).
fn pick_glyph(ch: char, fonts: &FontList) -> Result<(usize, u16, f32, bool)> {
    let mut advance_fallback: Option<(usize, u16, f32)> = None;

    for (fi, font) in fonts.fonts.iter().enumerate() {
        let face = font.face()?;
        let upem = face.units_per_em() as f32;
        if upem <= 0.0 {
            continue;
        }
        if let Some(gid) = face.glyph_index(ch) {
            let advance = face
                .glyph_hor_advance(gid)
                .map(|a| a as f32 / upem)
                .unwrap_or(0.0);

            if face.outline_glyph(gid, &mut Noop).is_some() {
                return Ok((fi, gid.0, advance, true));
            }
            if advance_fallback.is_none() && advance > 0.0 {
                advance_fallback = Some((fi, gid.0, advance));
            }
        }
    }

    let Some((fi, gid, advance)) = advance_fallback else {
        return Err(ChaminaError::GlyphOutline(ch));
    };
    Ok((fi, gid, advance, false))
}

/// Outline builder that records nothing: used to test whether a glyph has a
/// real outline without tessellating it.
struct Noop;

impl ttf_parser::OutlineBuilder for Noop {
    fn move_to(&mut self, _x: f32, _y: f32) {}
    fn line_to(&mut self, _x: f32, _y: f32) {}
    fn quad_to(&mut self, _x1: f32, _y1: f32, _x: f32, _y: f32) {}
    fn curve_to(&mut self, _x1: f32, _y1: f32, _x2: f32, _y2: f32, _x: f32, _y: f32) {}
    fn close(&mut self) {}
}
