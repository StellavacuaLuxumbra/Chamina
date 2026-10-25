//! Native offline renderer: `chamina-native <scene_data.json> [options]`
//!
//!   --audio-env <file>  spectrum envelope for the visualisers
//!   -o <file>           output mp4 (ffmpeg rawvideo pipe)
//!   --still <sec> --png <file>   render one frame to PNG (via ffmpeg)
//!   --from <n> --frames <n>      frame range
//!   --probe <t>...               print camera / glyph states (debug)

use chamina::native::{self, draw, pipe, post, NativeRenderer};
use std::io::Write;

struct Args {
    input: String,
    audio_env: Option<String>,
    out: Option<String>,
    still: Option<f32>,
    png: Option<String>,
    from: u32,
    frames: Option<u32>,
    probe: bool,
}

fn parse() -> Args {
    let mut a = Args {
        input: String::new(),
        audio_env: None,
        out: None,
        still: None,
        png: None,
        from: 0,
        frames: None,
        probe: false,
    };
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < argv.len() {
        match argv[i].as_str() {
            "--audio-env" => {
                i += 1;
                a.audio_env = Some(argv[i].clone());
            }
            "-o" | "--out" => {
                i += 1;
                a.out = Some(argv[i].clone());
            }
            "--still" => {
                i += 1;
                a.still = Some(argv[i].parse().expect("--still <seconds>"));
            }
            "--png" => {
                i += 1;
                a.png = Some(argv[i].clone());
            }
            "--from" => {
                i += 1;
                a.from = argv[i].parse().expect("--from <n>");
            }
            "--frames" => {
                i += 1;
                a.frames = Some(argv[i].parse().expect("--frames <n>"));
            }
            "--probe" => a.probe = true,
            other => {
                if other.starts_with('-') {
                    panic!("unknown flag {other}");
                }
                a.input = other.to_string();
            }
        }
        i += 1;
    }
    if a.input.is_empty() {
        eprintln!(
            "usage: chamina-native <scene_data.json> [--audio-env f] -o out.mp4 \
             [--from n] [--frames n] [--still s --png f]"
        );
        std::process::exit(1);
    }
    a
}

fn main() {
    if std::env::var("NATIVE_PROFILE").is_ok() {
        eprintln!("[prof] rayon threads: {}", rayon::current_num_threads());
    }
    let args = parse();
    let data =
        native::load_scene_data(std::path::Path::new(&args.input)).expect("cannot load scene_data");
    let env = args
        .audio_env
        .as_ref()
        .map(|p| native::AudioEnv::load(std::path::Path::new(p)).expect("audio env"));
    let r = NativeRenderer::new(data, env);
    let meta = &r.data.meta;
    let fps = meta.fps;
    let total = meta.total_frames;

    // Post chain from the scene's own settings.
    let dof = meta.dof.enabled.then(|| {
        let aperture = 0.00017 * (4.0 / meta.dof.aperture_f_stops.max(0.1));
        let maxblur = 0.008 * meta.dof.focal_radius.max(0.001);
        (meta.dof.focal_distance, aperture, maxblur)
    });
    let post_off = std::env::var("NATIVE_NOPOST").is_ok();
    let post_cfg = if post_off {
        post::PostConfig {
            bloom: None,
            dof: None,
            fxaa: false,
            fx: None,
        }
    } else {
        post::PostConfig {
            bloom: meta
                .bloom
                .enabled
                .then_some((meta.bloom.intensity, meta.bloom.threshold)),
            dof,
            fxaa: meta.fxaa,
            fx: meta.fx.enabled.then_some({
                (
                    meta.fx.chromatic,
                    meta.fx.vignette,
                    meta.fx.saturation,
                    0.0, // flash is per-frame, injected below via energy/hit
                )
            }),
        }
    };

    if args.probe {
        for t in [8.0f32, 17.0, 20.35] {
            let c = {
                let (bands, _b, energy) = r
                    .env
                    .as_ref()
                    .map(|e| e.sample(t))
                    .unwrap_or(([0.0; 64], 0.0, 0.0));
                let _ = bands;
                let fx = if r.data.meta.fx.enabled {
                    Some(&r.data.meta.fx)
                } else {
                    None
                };
                match &r.data.meta.camera_animation {
                    Some(chamina::scene::CameraAnim::Orbit(o)) => native::update_camera(
                        o,
                        t,
                        &r.cues,
                        fx,
                        energy,
                        r.data.meta.camera.fov,
                        r.data.meta.width as f32 / r.data.meta.height as f32,
                        r.data.meta.camera.near,
                    ),
                    _ => continue,
                }
            };
            println!(
                "t={t} eye {:?} target {:?} roll {:.3}",
                c.eye.to_array(),
                c.target.to_array(),
                c.roll
            );
            if t == 8.0 {
                if let Some(pal) = native::eval_backgrounds(&r.data.meta.backgrounds, t) {
                    println!("palette {:?}", &pal[..12]);
                    // left pixel (300,540) ray, straight from the camera basis
                    let ray = {
                        let aspect = r.data.meta.width as f32 / r.data.meta.height as f32;
                        let xn = 2.0 * 300.5 / r.data.meta.width as f32 - 1.0;
                        let yn = 1.0 - 2.0 * 540.5 / r.data.meta.height as f32;
                        (c.fwd + c.right * (xn * c.tan_h * aspect) + c.up * (yn * c.tan_h))
                            .normalize()
                    };
                    println!(
                        "sky left {:?}",
                        chamina::native::draw::skydome_pixel(ray, t, &pal)
                    );
                }
            }
        }
        return;
    }

    if let Some(t) = args.still {
        let frame = r.render_frame(t);
        let mut cfg = post_cfg.clone();
        // per-frame flash
        if meta.fx.enabled {
            let (_, _, energy) = r
                .env
                .as_ref()
                .map(|e| e.sample(t))
                .unwrap_or(([0.0; 64], 0.0, 0.0));
            let hit = native::hit_envelope(&r.cues, t, meta.fx.hit_duration);
            cfg.fx = Some((
                meta.fx.chromatic,
                meta.fx.vignette,
                meta.fx.saturation,
                meta.fx.flash * hit * (0.45 + 0.55 * energy),
            ));
        }
        let rgb = post::finish(&frame, &cfg);
        let out = args.png.clone().unwrap_or_else(|| "still.png".into());
        write_png(&out, &rgb, meta.width, meta.height);
        println!("still t={t} -> {out}");
        return;
    }

    let start = args.from;
    let count = args.frames.unwrap_or(total - start).min(total - start);
    let out_path = args.out.clone().unwrap_or_else(|| "native_out.mp4".into());

    let mut pipe = pipe::VideoPipe::open(&out_path, meta.width, meta.height, fps)
        .expect("cannot start ffmpeg");
    let t0 = std::time::Instant::now();
    for i in 0..count {
        let t = (start + i) as f32 / fps as f32;
        let frame = r.render_frame(t);
        let mut cfg = post_cfg.clone();
        if meta.fx.enabled {
            let (_, _, energy) = r
                .env
                .as_ref()
                .map(|e| e.sample(t))
                .unwrap_or(([0.0; 64], 0.0, 0.0));
            let hit = native::hit_envelope(&r.cues, t, meta.fx.hit_duration);
            cfg.fx = Some((
                meta.fx.chromatic,
                meta.fx.vignette,
                meta.fx.saturation,
                meta.fx.flash * hit * (0.45 + 0.55 * energy),
            ));
        }
        let rgb = post::finish(&frame, &cfg);
        pipe.write_frame(&rgb).expect("ffmpeg died");
        if (i + 1) % 60 == 0 || i + 1 == count {
            let el = t0.elapsed().as_secs_f32();
            eprintln!(
                "[native] frame {}/{}  {:.2} fps  elapsed {:.1}s  eta {:.0}s",
                i + 1,
                count,
                (i + 1) as f32 / el,
                el,
                el / (i + 1) as f32 * (count - i - 1) as f32
            );
        }
    }
    if let Err(e) = pipe.finish() {
        eprintln!("[native] {e}");
        std::process::exit(1);
    }
    let el = t0.elapsed().as_secs_f32();
    eprintln!(
        "[native] done -> {out_path}  {} frames  {:.2} fps  {:.1}s",
        count,
        count as f32 / el,
        el
    );
    let _ = draw::Face::Both; // keep the import alive for the probe path
}

fn write_png(path: &str, rgb: &[u8], w: u32, h: u32) {
    // Minimal PNG writer: RGB8, no filter, stored Deflate-free via ffmpeg.
    let ff = if let Ok(p) = std::env::var("CHAMINA_FFMPEG") {
        p
    } else if std::path::Path::new("js/node_modules/@ffmpeg-installer/win32-x64/ffmpeg.exe")
        .exists()
    {
        "js/node_modules/@ffmpeg-installer/win32-x64/ffmpeg.exe".into()
    } else {
        "ffmpeg".into()
    };
    use std::process::{Command, Stdio};
    let mut child = Command::new(ff)
        .args([
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-s",
            &format!("{}x{}", w, h),
            "-i",
            "pipe:0",
            path,
        ])
        .stdin(Stdio::piped())
        .spawn()
        .expect("ffmpeg for still");
    child
        .stdin
        .as_mut()
        .unwrap()
        .write_all(rgb)
        .expect("write still");
    child.wait().expect("ffmpeg still failed");
}
