// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::ffi::OsString;
use std::path::Path;

use anyhow::{Result, bail};

use tab5conv_core::jpeg::Frame;
use tab5conv_core::thumbnail::{self, Thumbnail};
use tab5conv_core::video::Picture;

use crate::args;
use crate::process::Tools;

pub struct Capture<'a> {
    pub tools: &'a Tools,
    pub input: &'a Path,
    pub index: u32,
    pub picture: &'a Picture,
    pub thumbnail: &'a Thumbnail,
}

impl Capture<'_> {
    /// One decoded frame as raw I420, which `core` turns into the JPEG. The
    /// seek is in front of `-i`, so a long input costs no more than a short
    /// one.
    pub fn args(&self, at: f64) -> Vec<OsString> {
        let size = (self.thumbnail.width, self.thumbnail.height);
        let mut args: Vec<OsString> = ["-hide_banner", "-loglevel", "error", "-nostdin"]
            .map(OsString::from)
            .to_vec();
        if at > 0.0 {
            args.extend(["-ss".into(), OsString::from(format!("{at:.3}"))]);
        }
        args.push("-i".into());
        args.push(self.input.into());
        args.extend(["-map".into(), format!("0:{}", self.index).into()]);
        args.extend(["-frames:v".into(), "1".into()]);
        args.extend([
            "-vf".into(),
            args::thumbnail_filter(self.picture, size).into(),
        ]);
        args.extend(["-pix_fmt".into(), "yuv420p".into()]);
        args.extend(["-f".into(), "rawvideo".into(), "pipe:1".into()]);
        args
    }

    pub fn command_line(&self) -> String {
        format!(
            "{} \\\n  | (built-in JPEG encoder)",
            self.tools.command_line(&self.args(self.thumbnail.at))
        )
    }

    pub fn run(&self) -> Result<Vec<u8>> {
        let (width, height) = (self.thumbnail.width, self.thumbnail.height);
        let needed = Frame::bytes(width as usize, height as usize);
        let mut frame = self.tools.output(&self.args(self.thumbnail.at))?;
        // A duration that is wrong, or a stream that ends before it says it
        // does, leaves the seek past the last frame and ffmpeg writes nothing.
        if frame.len() < needed && self.thumbnail.at > 0.0 {
            frame = self.tools.output(&self.args(0.0))?;
        }
        if frame.len() < needed {
            bail!("no frame at {:.1}s", self.thumbnail.at);
        }
        thumbnail::encode(width, height, &frame, self.thumbnail.quality)
    }
}
