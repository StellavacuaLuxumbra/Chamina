//! Scene → `scene_data.json`: the bridge consumed by the JS/Three.js renderer.
//!
//! Everything the renderer needs is in one JSON document: video metadata
//! (size, fps, frame count, camera, lights, post), and for every text line its
//! transform / material / animation specs plus one extruded mesh per visible
//! character.

use std::path::Path;

use serde::Serialize;

use crate::animation::AnimationSpec;
use crate::color::Color;
use crate::error::{ChaminaError, Result};
use crate::material::Material;
use crate::math::{Quat, Vec3};
use crate::scene::{
    BackgroundKey, BloomConfig, CameraAnim, CameraConfig, DofConfig, FxConfig, LightSetup, Scene,
};
use crate::text::{build_char_mesh, chain_for, layout_line};

/// The whole document handed to the renderer.
#[derive(Debug, Serialize)]
pub struct SceneData {
    pub meta: Meta,
    pub texts: Vec<TextExport>,
}

/// Video-level settings.
#[derive(Debug, Serialize)]
pub struct Meta {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub duration_secs: f32,
    pub total_frames: u32,
    pub background: Color,
    pub backgrounds: Vec<BackgroundKey>,
    pub fx: FxConfig,
    pub hdr: bool,
    pub fxaa: bool,
    pub bloom: BloomConfig,
    pub dof: DofConfig,
    pub camera: CameraConfig,
    pub camera_animation: Option<CameraAnim>,
    pub lights: LightSetup,
    pub seed: u64,
    pub work_dir: String,
    pub ffmpeg: Option<String>,
    pub font_fallback: Vec<String>,
}

/// One text line: placement + styling + its characters.
#[derive(Debug, Serialize)]
pub struct TextExport {
    pub index: usize,
    pub text: String,
    pub size: f32,
    pub depth: f32,
    pub position: Vec3,
    pub rotation: Quat,
    /// `scale * size`, applied to every vertex component.
    pub base_scale: Vec3,
    pub alpha: f32,
    pub material: Material,
    pub animations: Vec<AnimationSpec>,
    /// Total advance width of the line, in em.
    pub total_width_em: f32,
    /// Em-corrected vertical centre of the line (already applied).
    pub line_center_y: f32,
    pub chars: Vec<CharExport>,
}

/// One extruded glyph, ready to upload as a BufferGeometry.
#[derive(Debug, Serialize)]
pub struct CharExport {
    /// The character itself (as a string, so surrogates stay valid JSON).
    pub ch: String,
    /// Index inside the line — drives every `stagger` delay.
    pub idx: usize,
    pub advance_em: f32,
    /// World position of this glyph's mesh origin (bbox centre) before any
    /// animation is applied.
    pub base_position: Vec3,
    /// Emitted only for characters that have a real outline (spaces are not).
    pub vertices: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub indices: Vec<u32>,
}

/// Compute every glyph mesh of `scene`.
pub fn build(scene: &Scene) -> Result<SceneData> {
    let mut texts = Vec::with_capacity(scene.texts.len());

    for (ti, spec) in scene.texts.iter().enumerate() {
        let fonts = chain_for(
            spec.font.as_deref().map(|p| (p, spec.font_index)),
            &scene.font_fallback,
        )?;
        let layout = layout_line(&spec.text, &fonts)?;

        // Auto-fit. The advance sum only exists once the real font metrics are
        // in hand, so a width budget has to be resolved here: shrink `size`
        // until the line fits, then let every downstream consumer (mesh scale,
        // animation offsets, the exported `size`) use the resolved value.
        let mut size = spec.size;
        if let Some(max_w) = spec.fit_width {
            let w = layout.total_width * size;
            // A zero or negative budget would collapse (or mirror) the line;
            // treat those as "no budget" the same way the builder does.
            if w > max_w && max_w > 0.0 && w > 0.0 {
                size *= max_w / w;
            }
        }

        let base_scale = Vec3::new(
            spec.scale.x * size,
            spec.scale.y * size,
            spec.scale.z * size,
        );

        let mut chars = Vec::with_capacity(layout.glyphs.len());
        for (idx, g) in layout.glyphs.iter().enumerate() {
            if !g.has_outline {
                continue;
            }
            let mesh = build_char_mesh(g.ch, g.font_idx, g.glyph_id, spec.depth, &fonts)?;

            // Mesh vertices sit at `outline - origin`; put the origin back to
            // get outline space, then line-space, then world space.
            let local = Vec3::new(
                g.center.x + mesh.origin[0] - g.advance_em * 0.5,
                g.center.y + mesh.origin[1],
                0.0,
            ) * size;
            let base_position = spec.position + spec.rotation.mul_vec3(local);

            chars.push(CharExport {
                ch: g.ch.to_string(),
                idx,
                advance_em: g.advance_em,
                base_position,
                vertices: mesh.vertices,
                normals: mesh.normals,
                uvs: mesh.uvs,
                indices: mesh.indices,
            });
        }

        texts.push(TextExport {
            index: ti,
            text: spec.text.clone(),
            size,
            depth: spec.depth,
            position: spec.position,
            rotation: spec.rotation,
            base_scale,
            alpha: spec.alpha,
            material: spec.material,
            animations: spec.animations.clone(),
            total_width_em: layout.total_width,
            line_center_y: layout.line_center_y,
            chars,
        });
    }

    Ok(SceneData {
        meta: build_meta(scene),
        texts,
    })
}

fn build_meta(scene: &Scene) -> Meta {
    let duration_secs = scene.duration.as_secs_f32();
    let total_frames = ((duration_secs * scene.fps as f32).ceil() as u32).max(1);
    Meta {
        width: scene.width,
        height: scene.height,
        fps: scene.fps,
        duration_secs,
        total_frames,
        background: scene.background,
        backgrounds: scene.backgrounds.clone(),
        fx: scene.fx,
        hdr: scene.hdr,
        fxaa: scene.fxaa,
        bloom: scene.bloom,
        dof: scene.dof,
        camera: scene.camera,
        camera_animation: scene.camera_animation,
        lights: scene.lights,
        seed: scene.seed,
        work_dir: scene.work_dir.to_string_lossy().into_owned(),
        ffmpeg: scene
            .ffmpeg
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned()),
        font_fallback: scene.font_fallback.clone(),
    }
}

/// Build the bridge document and pretty-print it to `out`.
pub fn export(scene: &Scene, out: &Path) -> Result<()> {
    let data = build(scene)?;

    if let Some(parent) = out.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }

    let file = std::fs::File::create(out)?;
    let writer = std::io::BufWriter::new(file);
    serde_json::to_writer_pretty(writer, &data).map_err(|e| {
        ChaminaError::Other(format!("failed to serialize `{}`: {e}", out.display()))
    })?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scene::Scene;

    const SAMPLE: &str = "远离那些繁华悲伤 我们从此告别了过往";

    fn scene_with(size: f32, fit_width: Option<f32>) -> Scene {
        let mut scene = Scene::new(1920, 1080, 60);
        {
            let spec = scene.add_text(SAMPLE);
            spec.size = size;
            spec.fit_width = fit_width;
        }
        scene
    }

    #[test]
    fn without_fit_width_the_size_survives_export() {
        let data = build(&scene_with(1.7, None)).unwrap();
        assert_eq!(data.texts[0].size, 1.7);
    }

    #[test]
    fn fit_width_shrinks_a_line_that_would_overflow() {
        let natural = build(&scene_with(1.7, None)).unwrap();
        let n = &natural.texts[0];
        let budget = n.total_width_em * n.size * 0.5;

        let fitted = build(&scene_with(1.7, Some(budget))).unwrap();
        let f = &fitted.texts[0];
        assert!(f.size < n.size, "expected a shrink, got {}", f.size);
        // The whole point of resolving it here: downstream consumers see a
        // line whose real width sits on the budget.
        assert!(
            (f.total_width_em * f.size - budget).abs() < 1e-3,
            "{} != {}",
            f.total_width_em * f.size,
            budget
        );
    }

    #[test]
    fn fit_width_leaves_a_line_that_already_fits() {
        let natural = build(&scene_with(0.2, None)).unwrap();
        let budget = natural.texts[0].total_width_em * natural.texts[0].size * 4.0;

        let fitted = build(&scene_with(0.2, Some(budget))).unwrap();
        assert_eq!(fitted.texts[0].size, 0.2);
    }

    #[test]
    fn the_mesh_scale_follows_the_resolved_size() {
        let natural = build(&scene_with(1.7, None)).unwrap();
        let n = &natural.texts[0];
        let budget = n.total_width_em * n.size * 0.5;

        let fitted = build(&scene_with(1.7, Some(budget))).unwrap();
        let f = &fitted.texts[0];
        assert!(f.size > 0.0);
        // base_scale = spec.scale * size, so the ratio has to stay put.
        assert!(
            (f.base_scale.x / f.size - n.base_scale.x / n.size).abs() < 1e-5,
            "mesh scale drifted away from the resolved size"
        );
        // …and the glyphs must actually move with it.
        assert!(!f.chars.is_empty());
        assert!(f
            .chars
            .iter()
            .all(|c| c.vertices.iter().all(|v| v[0].is_finite())));
    }

    #[test]
    fn a_zero_or_negative_budget_disables_the_fit() {
        for bad in [0.0, -3.0] {
            let data = build(&scene_with(1.7, Some(bad))).unwrap();
            assert_eq!(data.texts[0].size, 1.7, "budget {bad} should be ignored");
        }
    }
}
