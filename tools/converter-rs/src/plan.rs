// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::ffi::OsString;
use std::path::Path;

use anyhow::{Result, bail};

use crate::probe::MediaInfo;

pub const MAX_SIDE: u32 = 1280;
const MAX_MACROBLOCKS: u32 = 3600;
const MAX_AUDIO_CHANNELS: u32 = 2;
const MAX_SAMPLE_RATE: u32 = 48000;
const KEYFRAME_INTERVAL_SECONDS: f64 = 2.0;
const FALLBACK_FPS: f64 = 30.0;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Container {
    Mp4,
    Mkv,
}

impl Container {
    pub fn from_path(path: &Path) -> Result<Self> {
        let ext = path
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase());
        match ext.as_deref() {
            Some("mp4" | "m4v" | "mov") => Ok(Self::Mp4),
            Some("mkv") => Ok(Self::Mkv),
            _ => bail!(
                "{}: output must be .mp4, .m4v, .mov or .mkv",
                path.display()
            ),
        }
    }
}

fn even(value: f64) -> u32 {
    ((value / 2.0).round() as u32 * 2).max(2)
}

fn macroblocks(width: u32, height: u32) -> u32 {
    width.div_ceil(16) * height.div_ceil(16)
}

pub fn output_size(display_width: f64, display_height: f64, long_side: u32) -> (u32, u32) {
    let long_side = long_side.min(MAX_SIDE) as f64;
    let mut scale = (long_side / display_width.max(display_height)).min(1.0);
    loop {
        let size = (even(display_width * scale), even(display_height * scale));
        if size.0 <= MAX_SIDE
            && size.1 <= MAX_SIDE
            && macroblocks(size.0, size.1) <= MAX_MACROBLOCKS
        {
            return size;
        }
        scale *= 0.99;
    }
}

pub struct Plan<'a> {
    info: &'a MediaInfo,
    container: Container,
    pub width: u32,
    pub height: u32,
}

impl<'a> Plan<'a> {
    pub fn new(info: &'a MediaInfo, long_side: u32, container: Container) -> Self {
        let (width, height) = output_size(
            info.video.display_width,
            info.video.display_height,
            long_side,
        );
        Self {
            info,
            container,
            width,
            height,
        }
    }

    fn keyframe_interval(&self) -> u32 {
        let fps = self.info.video.fps.unwrap_or(FALLBACK_FPS);
        ((fps * KEYFRAME_INTERVAL_SECONDS).round() as u32).max(1)
    }

    pub fn ffmpeg_args(&self, input: &Path, output: &Path, overwrite: bool) -> Vec<OsString> {
        let mut args = Args::default();
        args.push(&["-hide_banner", "-loglevel", "warning", "-stats"]);
        args.push(&[if overwrite { "-y" } else { "-n" }]);
        args.push(&["-i"]);
        args.push_path(input);

        args.push(&["-map", &format!("0:{}", self.info.video.index)]);
        args.push(&[
            "-vf",
            &format!("scale={}:{},setsar=1", self.width, self.height),
            "-c:v",
            "libx264",
            "-profile:v",
            "high",
            "-pix_fmt",
            "yuv420p",
            "-crf",
            "23",
            "-g",
            &self.keyframe_interval().to_string(),
        ]);

        if let Some(audio) = &self.info.audio {
            args.push(&[
                "-map",
                &format!("0:{}", audio.index),
                "-c:a",
                "aac",
                "-b:a",
                "128k",
            ]);
            if audio.channels > MAX_AUDIO_CHANNELS {
                args.push(&["-ac", &MAX_AUDIO_CHANNELS.to_string()]);
            }
            if audio.sample_rate > MAX_SAMPLE_RATE {
                args.push(&["-ar", &MAX_SAMPLE_RATE.to_string()]);
            }
        }

        if self.container == Container::Mp4 {
            args.push(&["-movflags", "+faststart"]);
        }

        args.push_path(output);
        args.0
    }
}

#[derive(Default)]
struct Args(Vec<OsString>);

impl Args {
    fn push(&mut self, items: &[&str]) {
        self.0.extend(items.iter().map(OsString::from));
    }

    fn push_path(&mut self, path: &Path) {
        self.0.push(path.into());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn landscape_is_fit_to_long_side() {
        assert_eq!(output_size(1920.0, 1080.0, 640), (640, 360));
    }

    #[test]
    fn portrait_is_fit_to_long_side() {
        assert_eq!(output_size(1080.0, 1920.0, 640), (360, 640));
    }

    #[test]
    fn small_source_is_not_upscaled() {
        assert_eq!(output_size(320.0, 240.0, 640), (320, 240));
    }

    #[test]
    fn odd_sizes_become_even() {
        assert_eq!(output_size(853.33, 480.0, 1280), (854, 480));
    }

    #[test]
    fn long_side_is_capped() {
        assert_eq!(output_size(3840.0, 2160.0, 4000), (1280, 720));
    }

    #[test]
    fn square_is_shrunk_to_macroblock_limit() {
        let (w, h) = output_size(1280.0, 1280.0, 1280);
        assert_eq!(w, h);
        assert!(macroblocks(w, h) <= MAX_MACROBLOCKS);
        assert!(w >= 950);
    }

    #[test]
    fn container_from_extension() {
        assert_eq!(
            Container::from_path(Path::new("a.MP4")).unwrap(),
            Container::Mp4
        );
        assert_eq!(
            Container::from_path(Path::new("a.mkv")).unwrap(),
            Container::Mkv
        );
        assert!(Container::from_path(Path::new("a.avi")).is_err());
    }
}
