//! Text → mesh pipeline: font loading, line layout and glyph extrusion.

pub mod font;
pub mod layout;
pub mod mesher;

pub use font::{FontList, FontReq, OwnedFont};
pub use layout::{layout_line, GlyphItem, TextLayout};
pub use mesher::{build_char_mesh, CharMesh};

/// Convenience: load the font chain for one [`crate::scene::TextSpec`].
pub fn chain_for(explicit: Option<(&str, u32)>, fallback: &[String]) -> crate::Result<FontList> {
    let mut reqs: Vec<FontReq> = Vec::new();
    if let Some((path, idx)) = explicit {
        reqs.push(FontReq {
            path: path.to_string(),
            index: idx,
        });
    }
    reqs.extend(fallback.iter().map(|p| FontReq::file(p.as_str())));
    FontList::load(&reqs)
}
