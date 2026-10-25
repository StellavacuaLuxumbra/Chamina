//! ffmpeg rawvideo pipe: the native renderer's answer to `render.js`'s PNG
//! stream. Same encoder flags, one less colour conversion.

use std::io::Write;
use std::process::{Child, Command, Stdio};

pub struct VideoPipe {
    child: Child,
}

fn find_ffmpeg() -> String {
    if let Ok(p) = std::env::var("CHAMINA_FFMPEG") {
        return p;
    }
    // Same installer the JS toolchain uses.
    {
        let rel = "js/node_modules/@ffmpeg-installer/win32-x64/ffmpeg.exe";
        let p = std::path::Path::new(rel);
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "ffmpeg".into()
}

impl VideoPipe {
    pub fn open(out: &str, w: u32, h: u32, fps: u32) -> std::io::Result<Self> {
        let child = Command::new(find_ffmpeg())
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
                "-framerate",
                &fps.to_string(),
                "-i",
                "pipe:0",
                "-c:v",
                "libx264",
                "-preset",
                "medium",
                "-crf",
                "18",
                "-pix_fmt",
                "yuv420p",
                "-vf",
                "scale=trunc(iw/2)*2:trunc(ih/2)*2",
                "-movflags",
                "+faststart",
                out,
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()?;
        Ok(Self { child })
    }

    pub fn write_frame(&mut self, rgb: &[u8]) -> std::io::Result<()> {
        if let Some(stdin) = self.child.stdin.as_mut() {
            stdin.write_all(rgb)?;
        }
        Ok(())
    }

    pub fn finish(mut self) -> std::io::Result<String> {
        drop(self.child.stdin.take());
        let out = self.child.wait()?;
        let mut err = String::new();
        use std::io::Read;
        if let Some(mut e) = self.child.stderr.take() {
            let _ = e.read_to_string(&mut err);
        }
        if out.success() {
            Ok(String::new())
        } else {
            Err(std::io::Error::other(format!("ffmpeg failed: {err}")))
        }
    }
}
