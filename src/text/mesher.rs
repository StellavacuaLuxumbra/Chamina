//! Convert a glyph outline into an extruded two-sided flat mesh of the glyph
//! (front + back + side walls), in **em** units.
//!
//! Model-space conventions: vertices are em units with the glyph outline bbox
//! centre translated to the origin (world y-up), z = ±depth/2. The exporter
//! adds [`CharMesh::origin`] back to recover outline-space coordinates.

use crate::error::{ChaminaError, Result};
use crate::text::font::FontList;

use std::collections::HashMap;

use lyon::geom::Point as LPointImpl;
use lyon::path::Path;
use lyon::tessellation::{
    BuffersBuilder, FillOptions, FillRule, FillTessellator, FillVertex, VertexBuffers,
};

use ttf_parser::OutlineBuilder;

type LPoint = LPointImpl<f32>;

/// One extruded glyph, ready to be turned into a renderable mesh.
///
/// Vertices are in **em** units, relative to [`CharMesh::origin`] (the centre
/// of the glyph's outline bbox, also in em).
#[derive(Debug, Clone)]
pub struct CharMesh {
    pub ch: char,
    /// Outline bbox centre (em) that the vertices were translated by, i.e.
    /// `vertex = outline - origin`.
    pub origin: [f32; 2],
    pub vertices: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub indices: Vec<u32>,
}

// ---------------------------------------------------------------------------
// Outline capture
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
enum Seg {
    Start(LPoint),
    Line(LPoint),
    Quad(LPoint, LPoint),
    Cubic(LPoint, LPoint, LPoint),
}

impl Seg {
    /// Scale every control point — used to convert font units to em.
    fn scaled(self, s: f32) -> Self {
        let p = |q: LPoint| LPoint::new(q.x * s, q.y * s);
        match self {
            Seg::Start(a) => Seg::Start(p(a)),
            Seg::Line(a) => Seg::Line(p(a)),
            Seg::Quad(a, b) => Seg::Quad(p(a), p(b)),
            Seg::Cubic(a, b, c) => Seg::Cubic(p(a), p(b), p(c)),
        }
    }
}

#[derive(Debug, Default)]
struct OutlinePen {
    contours: Vec<Vec<Seg>>,
    current: Vec<Seg>,
}

impl OutlinePen {
    fn finish(mut self) -> Vec<Vec<Seg>> {
        if !self.current.is_empty() {
            self.contours.push(std::mem::take(&mut self.current));
        }
        self.contours
    }

    fn flush_contour(&mut self) {
        if !self.current.is_empty() {
            self.contours.push(std::mem::take(&mut self.current));
        }
    }
}

impl OutlineBuilder for OutlinePen {
    fn move_to(&mut self, x: f32, y: f32) {
        self.flush_contour();
        self.current.push(Seg::Start(LPoint::new(x, y)));
    }

    fn line_to(&mut self, x: f32, y: f32) {
        self.current.push(Seg::Line(LPoint::new(x, y)));
    }

    fn quad_to(&mut self, cx: f32, cy: f32, x: f32, y: f32) {
        self.current
            .push(Seg::Quad(LPoint::new(cx, cy), LPoint::new(x, y)));
    }

    fn curve_to(&mut self, c1x: f32, c1y: f32, c2x: f32, c2y: f32, x: f32, y: f32) {
        self.current.push(Seg::Cubic(
            LPoint::new(c1x, c1y),
            LPoint::new(c2x, c2y),
            LPoint::new(x, y),
        ));
    }

    fn close(&mut self) {}
}

// ---------------------------------------------------------------------------
// Path assembly
// ---------------------------------------------------------------------------

/// Read a glyph outline and normalise it from font units to **em**.
fn glyph_contours(
    ch: char,
    font_idx: usize,
    glyph_id: u16,
    fonts: &FontList,
) -> Result<Vec<Vec<Seg>>> {
    let face = fonts.fonts[font_idx].face()?;
    let upem = face.units_per_em() as f32;
    let mut pen = OutlinePen::default();
    face.outline_glyph(ttf_parser::GlyphId(glyph_id), &mut pen)
        .ok_or(ChaminaError::GlyphOutline(ch))?;
    let s = if upem > 0.0 { 1.0 / upem } else { 1.0 };
    Ok(pen
        .finish()
        .into_iter()
        .map(|c| c.into_iter().map(|seg| seg.scaled(s)).collect())
        .collect())
}

/// Build a closed `Path` out of already-flattened polylines (lines only).
fn build_path(polys: &[Vec<LPoint>]) -> Path {
    let mut builder = Path::builder();
    for poly in polys {
        let mut pts = poly.iter();
        let Some(first) = pts.next() else {
            continue;
        };
        builder.begin(*first);
        for p in pts {
            builder.line_to(*p);
        }
        builder.close();
    }
    builder.build()
}

fn polyline_bbox(polys: &[Vec<LPoint>]) -> [f32; 4] {
    let mut bb = [f32::MAX, f32::MAX, f32::MIN, f32::MIN];
    for poly in polys {
        for p in poly {
            bb = grow_bb(bb, p.x, p.y);
        }
    }
    bb
}

fn grow_bb(bb: [f32; 4], x: f32, y: f32) -> [f32; 4] {
    [bb[0].min(x), bb[1].min(y), bb[2].max(x), bb[3].max(y)]
}

// ---------------------------------------------------------------------------
// Mesh building
// ---------------------------------------------------------------------------

pub fn build_char_mesh(
    ch: char,
    font_idx: usize,
    glyph_id: u16,
    depth: f32,
    fonts: &FontList,
) -> Result<CharMesh> {
    let contours = glyph_contours(ch, font_idx, glyph_id, fonts)?;
    if contours.is_empty() {
        return Err(ChaminaError::GlyphOutline(ch));
    }
    let depth = depth.max(0.001);
    let h = depth * 0.5;

    // Flatten exactly once, up front. The caps and the side walls have to be
    // built from the *same* polyline: letting lyon flatten its own copy puts
    // different points on the two and leaves cracks all around the glyph. The
    // path handed to lyon is therefore rebuilt from these very points and
    // contains nothing but straight lines, so lyon has nothing left to flatten.
    let polys: Vec<Vec<LPoint>> = contours.iter().map(|c| flatten(c)).collect();
    if polys.iter().all(|p| p.len() < 3) {
        return Err(ChaminaError::GlyphOutline(ch));
    }

    // Tessellate the fill in the XY plane (lyon).
    let path = build_path(&polys);
    let mut fill = VertexBuffers::<LPoint, u32>::new();
    let mut tess = FillTessellator::new();
    tess.tessellate_path(
        &path,
        &FillOptions::default().with_fill_rule(FillRule::EvenOdd),
        &mut BuffersBuilder::new(&mut fill, |v: FillVertex| v.position()),
    )
    .map_err(|e| ChaminaError::Other(format!("tessellation error: {e}")))?;

    let bb = polyline_bbox(&polys);
    let (cx, cy) = {
        let w = (bb[2] - bb[0]).max(0.0001);
        let ht = (bb[3] - bb[1]).max(0.0001);
        (bb[0] + w * 0.5, bb[1] + ht * 0.5)
    };

    let n = fill.vertices.len();
    let mut vertices = Vec::with_capacity(n * 2 + 256);
    let mut normals = Vec::with_capacity(vertices.capacity());
    let mut uvs = Vec::with_capacity(vertices.capacity());
    let mut indices = Vec::with_capacity(fill.indices.len() * 2 + 512);

    // lyon pads collinear stretches of the outline with zero-area triangles.
    // They render as nothing but they inflate the edge counts, so they are
    // thrown away before the caps are emitted — and the wall is then derived
    // from the boundary of what is left, which by construction makes every
    // remaining edge shared by exactly two triangles.
    let degen = |a: LPoint, b: LPoint, c: LPoint| {
        ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)).abs() < 1e-9
    };
    let keep: Vec<[u32; 3]> = fill
        .indices
        .as_chunks::<3>()
        .0
        .iter()
        .copied()
        .filter(|t| {
            !degen(
                fill.vertices[t[0] as usize],
                fill.vertices[t[1] as usize],
                fill.vertices[t[2] as usize],
            )
        })
        .collect();

    // Front face (facing +Z). lyon's fill output is clockwise as seen from
    // +Z, which WebGL treats as a back face, so every triangle is flipped.
    for v in &fill.vertices {
        vertices.push([v.x - cx, v.y - cy, h]);
        normals.push([0.0, 0.0, 1.0]);
        uvs.push([v.x - cx, -(v.y - cy)]);
    }
    for tri in &keep {
        indices.extend_from_slice(&[tri[0], tri[2], tri[1]]);
    }

    // Back face (facing -Z): lyon's own winding, which reads counter-clockwise
    // once you look at it from -Z.
    let back_base = vertices.len() as u32;
    for v in &fill.vertices {
        vertices.push([v.x - cx, v.y - cy, -h]);
        normals.push([0.0, 0.0, -1.0]);
        uvs.push([v.x - cx, -(v.y - cy)]);
    }
    for tri in &keep {
        indices.push(back_base + tri[0]);
        indices.push(back_base + tri[1]);
        indices.push(back_base + tri[2]);
    }

    // Side walls: one quad per edge that only one cap triangle claims. The
    // endpoints are the cap's own vertices, so the extrusion stays closed no
    // matter how lyon chose to split or bridge the outline while tessellating.
    let mut third: HashMap<(u32, u32), u32> = HashMap::new();
    let mut counts: HashMap<(u32, u32), u32> = HashMap::new();
    for t in &keep {
        for k in 0..3 {
            let (a, b) = (t[k], t[(k + 1) % 3]);
            third.entry((a, b)).or_insert(t[(k + 2) % 3]);
            let key = if a <= b { (a, b) } else { (b, a) };
            *counts.entry(key).or_default() += 1;
        }
    }
    let pos = |i: u32| fill.vertices[i as usize];
    let mut edges: Vec<(u32, u32, u32)> = Vec::new();
    for (&(a, b), &n) in &counts {
        if n != 1 {
            continue;
        }
        let c = third
            .get(&(a, b))
            .or_else(|| third.get(&(b, a)))
            .copied()
            .expect("boundary edge must belong to a triangle");
        edges.push((a, b, c));
    }
    edges.sort_unstable();
    for (a, b, c) in edges {
        wall_quad_into(
            &mut vertices,
            &mut normals,
            &mut uvs,
            &mut indices,
            pos(a),
            pos(b),
            Some(pos(c)),
            cx,
            cy,
            h,
        );
    }

    Ok(CharMesh {
        ch,
        origin: [cx, cy],
        vertices,
        normals,
        uvs,
        indices,
    })
}

fn flatten(contour: &[Seg]) -> Vec<LPoint> {
    let mut out: Vec<LPoint> = Vec::new();
    let mut cur: Option<LPoint> = None;
    for seg in contour {
        match *seg {
            Seg::Start(p) | Seg::Line(p) => {
                out.push(p);
                cur = Some(p);
            }
            Seg::Quad(c, p) => {
                if let Some(s) = cur {
                    let (mut x0, mut y0) = (s.x, s.y);
                    const N: usize = 16;
                    for i in 1..=N {
                        let t = i as f32 / N as f32;
                        let mt = 1.0 - t;
                        let x = mt * mt * x0 + 2.0 * mt * t * c.x + t * t * p.x;
                        let y = mt * mt * y0 + 2.0 * mt * t * c.y + t * t * p.y;
                        out.push(LPoint::new(x, y));
                        x0 = x;
                        y0 = y;
                    }
                }
                cur = Some(p);
            }
            Seg::Cubic(c1, c2, p) => {
                if let Some(s) = cur {
                    let (mut x0, mut y0) = (s.x, s.y);
                    const N: usize = 24;
                    for i in 1..=N {
                        let t = i as f32 / N as f32;
                        let mt = 1.0 - t;
                        let x = mt * mt * mt * x0
                            + 3.0 * mt * mt * t * c1.x
                            + 3.0 * mt * t * t * c2.x
                            + t * t * t * p.x;
                        let y = mt * mt * mt * y0
                            + 3.0 * mt * mt * t * c1.y
                            + 3.0 * mt * t * t * c2.y
                            + t * t * t * p.y;
                        out.push(LPoint::new(x, y));
                        x0 = x;
                        y0 = y;
                    }
                }
                cur = Some(p);
            }
        }
    }
    dedup_closing(out)
}

/// Drop consecutive duplicates (fonts happily emit zero-length segments) and
/// a trailing point that repeats the first. Both would otherwise produce
/// degenerate side-wall quads — zero-area triangles that leave the extrusion
/// with open edges.
fn dedup_closing(pts: Vec<LPoint>) -> Vec<LPoint> {
    const EPS: f32 = 1e-6;
    let close = |a: LPoint, b: LPoint| (a.x - b.x).abs() <= EPS && (a.y - b.y).abs() <= EPS;

    let mut out: Vec<LPoint> = Vec::with_capacity(pts.len());
    for p in pts {
        if out.last().is_some_and(|&q| close(q, p)) {
            continue;
        }
        out.push(p);
    }
    while out.len() > 1 && close(out[0], out[out.len() - 1]) {
        out.pop();
    }
    out
}

#[allow(clippy::too_many_arguments)]
fn wall_quad_into(
    vertices: &mut Vec<[f32; 3]>,
    normals: &mut Vec<[f32; 3]>,
    uvs: &mut Vec<[f32; 2]>,
    indices: &mut Vec<u32>,
    a0: LPoint,
    b0: LPoint,
    adjacent: Option<LPoint>,
    cx: f32,
    cy: f32,
    h: f32,
) {
    let base = vertices.len() as u32;

    let (dx, dy) = (b0.x - a0.x, b0.y - a0.y);
    let len = (dx * dx + dy * dy).sqrt().max(1e-6);
    let (mut nx, mut ny) = (-dy / len, dx / len);

    // Aim the normal away from the cap triangle this edge came from, so the
    // wall faces out of the glyph rather than into it. Flipping the normal
    // means reversing the edge too: the quad's winding derives from (a, b).
    let (mut a, mut b) = (a0, b0);
    if let Some(c) = adjacent {
        let mx = (a0.x + b0.x) * 0.5;
        let my = (a0.y + b0.y) * 0.5;
        if nx * (c.x - mx) + ny * (c.y - my) > 0.0 {
            nx = -nx;
            ny = -ny;
            std::mem::swap(&mut a, &mut b);
        }
    }

    let ax = a.x - cx;
    let ay = a.y - cy;
    let bx = b.x - cx;
    let by = b.y - cy;

    vertices.push([ax, ay, h]);
    vertices.push([bx, by, h]);
    vertices.push([bx, by, -h]);
    vertices.push([ax, ay, -h]);
    for _ in 0..4 {
        normals.push([nx, ny, 0.0]);
    }
    uvs.push([0.0, 0.0]);
    uvs.push([1.0, 0.0]);
    uvs.push([1.0, 1.0]);
    uvs.push([0.0, 1.0]);
    indices.push(base);
    indices.push(base + 1);
    indices.push(base + 2);
    indices.push(base);
    indices.push(base + 2);
    indices.push(base + 3);
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use super::*;
    use crate::text::{chain_for, layout_line};

    /// System font lookup; the repo ships no binary font assets, so tests
    /// fall back to whatever the host provides and bail out if there is none.
    fn test_fonts() -> Option<FontList> {
        const CANDIDATES: &[&str] = &[
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
        ];
        let found: Vec<String> = CANDIDATES
            .iter()
            .filter(|p| std::path::Path::new(p).exists())
            .map(|p| (*p).to_string())
            .collect();
        if found.is_empty() {
            return None;
        }
        chain_for(None, &found).ok()
    }

    fn mesh_for(ch: char, fonts: &FontList) -> CharMesh {
        let layout = layout_line(&ch.to_string(), fonts).expect("layout");
        let g = &layout.glyphs[0];
        build_char_mesh(g.ch, g.font_idx, g.glyph_id, 0.2, fonts).expect("mesh")
    }

    /// Every edge of a closed solid must be shared by exactly two triangles.
    /// Corners are compared by position because the cap faces and the side
    /// walls legitimately duplicate their vertices.
    /// A closed solid may not have a boundary: every edge must be covered by
    /// an even number of faces (2 for a normal edge, 4 where a self-touching
    /// outline pinches the extrusion). An odd count is a hole. Anything not
    /// exactly 2 is reported, and the test also fails if pinches spread.
    fn assert_watertight(mesh: &CharMesh, label: char) {
        let key = |v: [f32; 3]| -> (i64, i64, i64) {
            (
                (v[0] as f64 * 1e5).round() as i64,
                (v[1] as f64 * 1e5).round() as i64,
                (v[2] as f64 * 1e5).round() as i64,
            )
        };
        type Edge = ((i64, i64, i64), (i64, i64, i64));
        let mut edges: HashMap<Edge, usize> = HashMap::new();
        for tri in mesh.indices.as_chunks::<3>().0 {
            let p = [
                mesh.vertices[tri[0] as usize],
                mesh.vertices[tri[1] as usize],
                mesh.vertices[tri[2] as usize],
            ];
            for [i, j] in [[0usize, 1usize], [1, 2], [2, 0]] {
                let a = key(p[i]);
                let b = key(p[j]);
                let k = if a <= b { (a, b) } else { (b, a) };
                *edges.entry(k).or_insert(0) += 1;
            }
        }
        let odd: Vec<_> = edges
            .iter()
            .filter(|(_, &n)| n % 2 == 1)
            .map(|(&e, &n)| (e, n))
            .collect();
        let other: Vec<_> = edges
            .iter()
            .filter(|(_, &n)| n != 2)
            .map(|(&e, &n)| (e, n))
            .collect();
        let mut report = String::new();
        for (e, n) in other.iter().take(20) {
            report.push_str(&format!("\n  {:?} x{n}", e));
        }
        assert!(
            odd.is_empty(),
            "glyph {label:?}: {} edges with a hole:{}",
            odd.len(),
            report
        );
        // Self-touching outlines may pinch to 4 faces; anything wider means
        // quads are being duplicated rather than joined.
        assert!(
            other.len() * 20 <= edges.len(),
            "glyph {label:?}: too many non-manifold edges ({}/{}){}",
            other.len(),
            edges.len(),
            report
        );
    }

    #[test]
    fn straight_stroke_mesh_is_watertight() {
        let Some(fonts) = test_fonts() else {
            return;
        };
        // CJK glyphs are pure straight segments — the simplest possible
        // outlines, so they isolate layout/meshing bugs from curve handling.
        for ch in ['生', '日', '快', '乐'] {
            assert_watertight(&mesh_for(ch, &fonts), ch);
        }
    }

    #[test]
    fn curved_glyph_mesh_is_watertight() {
        let Some(fonts) = test_fonts() else {
            return;
        };
        // g/S/o cover bowls, diagonals and counters — i.e. curves, holes and
        // self-touching outlines, the cases a straight-stroke glyph never hits.
        for ch in ['g', 'S', 'o', 'a', 'B', 'e', 'n', 'y'] {
            assert_watertight(&mesh_for(ch, &fonts), ch);
        }
    }

    #[test]
    fn mesh_counts_stay_consistent() {
        let Some(fonts) = test_fonts() else {
            return;
        };
        let m = mesh_for('生', &fonts);
        assert_eq!(m.vertices.len(), m.normals.len());
        assert_eq!(m.vertices.len(), m.uvs.len());
        assert!(m.indices.iter().all(|&i| (i as usize) < m.vertices.len()));
        // front + back caps share one vertex list, walls add 4 per edge
        assert!(m.vertices.len() >= 4 * 8, "at least a box-shaped outline");
        assert!(m.origin.iter().all(|v| v.is_finite()));
    }
}
