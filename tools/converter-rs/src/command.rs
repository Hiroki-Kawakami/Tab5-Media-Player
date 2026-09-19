// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::ffi::OsString;
use std::path::Path;

use anyhow::{Result, bail};

use crate::audio::AudioPlan;
use crate::video::{MjpegJob, VideoOutput, VideoPlan};

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

pub struct Job<'a> {
    pub input: &'a Path,
    pub output: &'a Path,
    pub container: Container,
    pub overwrite: bool,
    pub video: &'a VideoPlan,
    pub audio: &'a AudioPlan,
}

pub enum Commands<'a> {
    Single(Vec<OsString>),
    Piped {
        decode: Vec<OsString>,
        mux: Vec<OsString>,
        job: &'a MjpegJob,
    },
}

fn strings<const N: usize>(items: [&str; N]) -> impl Iterator<Item = OsString> {
    items.into_iter().map(OsString::from)
}

impl<'a> Job<'a> {
    pub fn commands(&self) -> Commands<'a> {
        match &self.video.output {
            VideoOutput::Ffmpeg(video_args) => {
                let mut args = self.head();
                args.extend(strings(["-i"]));
                args.push(self.input.into());
                args.extend(["-map".into(), format!("0:{}", self.video.index).into()]);
                args.extend(video_args.iter().map(OsString::from));
                self.tail(&mut args, 0);
                Commands::Single(args)
            }
            VideoOutput::Mjpeg(job) => {
                let mut decode: Vec<OsString> = strings([
                    "-hide_banner",
                    "-loglevel",
                    "warning",
                    "-nostats",
                    "-nostdin",
                    "-i",
                ])
                .collect();
                decode.push(self.input.into());
                decode.extend(["-map".into(), format!("0:{}", self.video.index).into()]);
                decode.extend(["-vf".into(), job.filter.clone().into()]);
                decode.extend(strings(["-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"]));

                let mut mux = self.head();
                mux.extend(strings(["-f", "mjpeg", "-framerate"]));
                mux.push(job.rate.ffmpeg_value().into());
                mux.extend(strings(["-i", "pipe:0", "-i"]));
                mux.push(self.input.into());
                mux.extend(strings(["-map", "0:v", "-c:v", "copy"]));
                self.tail(&mut mux, 1);
                Commands::Piped {
                    decode,
                    mux,
                    job: match &self.video.output {
                        VideoOutput::Mjpeg(job) => job,
                        VideoOutput::Ffmpeg(_) => unreachable!(),
                    },
                }
            }
        }
    }

    fn head(&self) -> Vec<OsString> {
        strings([
            "-hide_banner",
            "-loglevel",
            "warning",
            "-stats",
            if self.overwrite { "-y" } else { "-n" },
        ])
        .collect()
    }

    fn tail(&self, args: &mut Vec<OsString>, audio_input: usize) {
        if let Some(index) = self.audio.index {
            args.extend(["-map".into(), format!("{audio_input}:{index}").into()]);
            args.extend(self.audio.args.iter().map(OsString::from));
        }
        if self.container == Container::Mp4 {
            args.extend(strings(["-movflags", "+faststart"]));
        }
        args.push(self.output.into());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert!(Container::from_path(Path::new("a")).is_err());
    }
}
