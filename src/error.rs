use std::path::PathBuf;

#[derive(Debug, thiserror::Error)]
pub enum ChaminaError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("failed to read font file `{0}`")]
    FontRead(PathBuf),

    #[error("failed to parse font `{0}` (not a TTF/OTF/collection?)")]
    FontFormat(PathBuf),

    #[error("no usable font was loaded: pass a font path (e.g. `--font ...`) or set the CHAMINA_FONT env var")]
    NoFont,

    #[error("glyph '{0}' has no outline in any loaded font")]
    GlyphOutline(char),

    #[error("ffmpeg was not found on PATH (install ffmpeg or set `scene.ffmpeg`)")]
    FfmpegNotFound,

    #[error("ffmpeg failed: {0}")]
    FfmpegFailed(String),

    #[error("{0}")]
    Other(String),
}

pub type Result<T> = std::result::Result<T, ChaminaError>;
