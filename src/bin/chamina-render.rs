//! `chamina-render` — one command from a scene spec to a finished MP4.
//!
//! ```text
//! chamina-render scene.json -o gift.mp4
//! ```
//!
//! Pipeline:
//! 1. load the scene spec (JSON) into a `chamina::Scene`
//! 2. compute every glyph mesh and write `scene_data.json`
//! 3. hand `scene_data.json` to the Node/Three.js renderer, which renders each
//!    frame and pipes it to ffmpeg.

use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("chamina-render: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let opts = Options::parse(&args)?;

    if opts.help {
        print!("{}", USAGE);
        return Ok(());
    }

    let spec_path = opts
        .scene
        .as_deref()
        .ok_or_else(|| format!("missing scene spec\n\n{USAGE}"))?;

    // 1. Load the scene ---------------------------------------------------
    let raw = std::fs::read_to_string(spec_path)
        .map_err(|e| format!("cannot read `{}`: {e}", spec_path.display()))?;
    let mut scene: chamina::scene::Scene = serde_json::from_str(&raw)
        .map_err(|e| format!("`{}` is not a valid scene spec: {e}", spec_path.display()))?;

    if let Some(dir) = &opts.work_dir {
        scene.work_dir = dir.clone();
    }
    std::fs::create_dir_all(&scene.work_dir)
        .map_err(|e| format!("cannot create work dir `{}`: {e}", scene.work_dir.display()))?;

    // 2. Export the bridge document --------------------------------------
    let data_path = opts
        .data
        .clone()
        .unwrap_or_else(|| scene.work_dir.join("scene_data.json"));
    scene
        .export(&data_path)
        .map_err(|e| format!("export failed: {e}"))?;
    eprintln!(
        "chamina-render: wrote {} ({} text line{})",
        data_path.display(),
        scene.texts.len(),
        if scene.texts.len() == 1 { "" } else { "s" }
    );

    if opts.dry_run {
        return Ok(());
    }

    // 3. Render ------------------------------------------------------------
    let output = opts
        .output
        .clone()
        .unwrap_or_else(|| scene.work_dir.join("chamina.mp4"));
    if let Some(parent) = output.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)
                .map_err(|e| format!("cannot create `{}`: {e}", parent.display()))?;
        }
    }

    let renderer = resolve_renderer(opts.renderer.as_deref())?;
    let node = opts.node.as_deref().unwrap_or("node");

    eprintln!(
        "chamina-render: rendering {} frames -> {}",
        ((scene.duration.as_secs_f32() * scene.fps as f32).ceil() as u32).max(1),
        output.display()
    );

    let mut cmd = Command::new(node);
    cmd.arg(&renderer)
        .arg("--data")
        .arg(&data_path)
        .arg("--out")
        .arg(&output);
    if let Some(audio) = &opts.audio {
        cmd.arg("--audio").arg(audio);
    }
    let status = cmd
        .status()
        .map_err(|e| format!("failed to launch `{node}`: {e}\n(is Node.js on PATH?)"))?;

    if !status.success() {
        return Err(format!(
            "renderer exited with {status}\n  node {} --data {} --out {}",
            renderer.display(),
            data_path.display(),
            output.display()
        ));
    }

    eprintln!("chamina-render: done -> {}", output.display());
    Ok(())
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

const USAGE: &str = "\
chamina-render — turn a chamina scene spec into a video

USAGE:
    chamina-render <scene.json> [OPTIONS]

OPTIONS:
    -o, --output <file>    output video (default: <work_dir>/chamina.mp4)
        --data <file>      bridge document (default: <work_dir>/scene_data.json)
        --work-dir <dir>   intermediate directory (default: from the scene spec)
        --renderer <file>  path to the JS renderer (render.js)
        --node <exe>       node executable (default: node)
        --audio <file>     mux this audio track into the output video
        --dry-run          only write scene_data.json, do not render
    -h, --help             print this help
";

#[derive(Debug, Default)]
struct Options {
    scene: Option<PathBuf>,
    output: Option<PathBuf>,
    data: Option<PathBuf>,
    work_dir: Option<PathBuf>,
    renderer: Option<PathBuf>,
    node: Option<String>,
    audio: Option<PathBuf>,
    dry_run: bool,
    help: bool,
}

impl Options {
    fn parse(args: &[String]) -> Result<Self, String> {
        let mut o = Options::default();
        let mut i = 0;
        while i < args.len() {
            let a = args[i].as_str();
            match a {
                "-h" | "--help" => o.help = true,
                "--dry-run" => o.dry_run = true,
                "-o" | "--output" => {
                    i += 1;
                    let v = args.get(i).ok_or_else(|| format!("{a} needs a value"))?;
                    o.output = Some(PathBuf::from(v));
                }
                "--data" => {
                    i += 1;
                    let v = args.get(i).ok_or_else(|| format!("{a} needs a value"))?;
                    o.data = Some(PathBuf::from(v));
                }
                "--work-dir" => {
                    i += 1;
                    let v = args.get(i).ok_or_else(|| format!("{a} needs a value"))?;
                    o.work_dir = Some(PathBuf::from(v));
                }
                "--renderer" => {
                    i += 1;
                    let v = args.get(i).ok_or_else(|| format!("{a} needs a value"))?;
                    o.renderer = Some(PathBuf::from(v));
                }
                "--node" => {
                    i += 1;
                    let v = args.get(i).ok_or_else(|| format!("{a} needs a value"))?;
                    o.node = Some(v.clone());
                }
                "--audio" => {
                    i += 1;
                    let v = args.get(i).ok_or_else(|| format!("{a} needs a value"))?;
                    let p = PathBuf::from(v);
                    if !p.is_file() {
                        return Err(format!("audio file `{}` does not exist", p.display()));
                    }
                    o.audio = Some(p);
                }
                _ if a.starts_with('-') => {
                    return Err(format!("unknown option `{a}`\n\n{USAGE}"));
                }
                _ => {
                    if o.scene.is_some() {
                        return Err(format!("unexpected extra argument `{a}`\n\n{USAGE}"));
                    }
                    o.scene = Some(PathBuf::from(a));
                }
            }
            i += 1;
        }
        Ok(o)
    }
}

// ---------------------------------------------------------------------------
// Renderer discovery
// ---------------------------------------------------------------------------

fn resolve_renderer(explicit: Option<&Path>) -> Result<PathBuf, String> {
    if let Some(p) = explicit {
        return check(p.to_path_buf());
    }
    if let Ok(p) = std::env::var("CHAMINA_RENDERER") {
        if !p.is_empty() {
            return check(PathBuf::from(p));
        }
    }

    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        push_ancestors(&mut candidates, &cwd, "render.js");
    }
    push_ancestors(
        &mut candidates,
        Path::new(env!("CARGO_MANIFEST_DIR")),
        "render.js",
    );
    candidates.push(PathBuf::from("js/render.js"));
    candidates.push(PathBuf::from("render.js"));

    for c in &candidates {
        if c.is_file() {
            return Ok(c.clone());
        }
    }
    Err(
        "cannot find the JS renderer (render.js); pass --renderer <file> or set CHAMINA_RENDERER"
            .to_string(),
    )
}

/// Push `<dir>/render.js`, `<dir>/js/render.js` and the same for every parent.
fn push_ancestors(out: &mut Vec<PathBuf>, dir: &Path, file: &str) {
    let mut cur = Some(dir);
    while let Some(d) = cur {
        out.push(d.join(file));
        out.push(d.join("js").join(file));
        cur = d.parent();
    }
}

fn check(p: PathBuf) -> Result<PathBuf, String> {
    if p.is_file() {
        Ok(p)
    } else {
        Err(format!("renderer `{}` does not exist", p.display()))
    }
}
