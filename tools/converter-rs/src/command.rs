// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::ffi::OsString;
use std::path::Path;

use anyhow::{Result, bail};

use crate::audio::AudioPlan;
use crate::video::VideoPlan;

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

impl Job<'_> {
    pub fn ffmpeg_args(&self) -> Vec<OsString> {
        let mut args: Vec<OsString> = Vec::new();
        let mut push = |items: &[&str]| args.extend(items.iter().map(OsString::from));
        push(&["-hide_banner", "-loglevel", "warning", "-stats"]);
        push(&[if self.overwrite { "-y" } else { "-n" }]);
        push(&["-i"]);
        args.push(self.input.into());

        args.extend(["-map".into(), format!("0:{}", self.video.index).into()]);
        args.extend(self.video.args.iter().map(OsString::from));
        if let Some(index) = self.audio.index {
            args.extend(["-map".into(), format!("0:{index}").into()]);
            args.extend(self.audio.args.iter().map(OsString::from));
        }
        if self.container == Container::Mp4 {
            args.extend(["-movflags".into(), "+faststart".into()]);
        }
        args.push(self.output.into());
        args
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
