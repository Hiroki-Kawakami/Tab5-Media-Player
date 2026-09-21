// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod args;
pub mod batch;
mod command;
mod pipeline;
mod probe;
mod process;
mod thumbnail;

use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use tab5conv_core::Specs;
use tab5conv_core::audio::AudioPlan;
use tab5conv_core::container::Container;
use tab5conv_core::media::MediaInfo;
use tab5conv_core::thumbnail::Thumbnail;
use tab5conv_core::video::{VideoCodec, VideoPlan};

use thumbnail::Capture;

pub use probe::probe;
pub use process::{Cancelled, Monitor, Tools};
pub use tab5conv_core::mjpeg::Stats;

pub struct Conversion {
    tools: Tools,
    input: PathBuf,
    output: PathBuf,
    container: Container,
    pub info: MediaInfo,
    pub video: VideoPlan,
    pub audio: AudioPlan,
    pub thumbnail: Option<Thumbnail>,
    pub thumbnail_label: String,
}

impl Conversion {
    pub fn prepare(
        tools: &Tools,
        input: &Path,
        output: &Path,
        container: Container,
        specs: &Specs,
    ) -> Result<Self> {
        let info = probe::probe(tools, input)?;
        let video = specs.video.plan(&info.video)?;
        let audio = specs.audio.plan(info.audio.as_ref())?;
        let thumbnail = specs.thumbnail.plan(&video.picture, info.duration);
        tools.require_encoders(&args::encoders(&video, &audio))?;
        Ok(Self {
            tools: tools.clone(),
            input: input.to_path_buf(),
            output: output.to_path_buf(),
            container,
            info,
            video,
            audio,
            thumbnail_label: specs.thumbnail.label(thumbnail.as_ref()),
            thumbnail,
        })
    }

    fn capture(&self) -> Option<Capture<'_>> {
        self.thumbnail.as_ref().map(|thumbnail| Capture {
            tools: &self.tools,
            input: &self.input,
            index: self.video.index,
            picture: &self.video.picture,
            thumbnail,
        })
    }

    /// The built-in mp4 muxer writes the cover itself; every other output is
    /// muxed by ffmpeg, which wants it as a file.
    fn self_muxed(&self) -> bool {
        self.container == Container::Mp4 && matches!(self.video.codec, VideoCodec::Mjpeg(_))
    }

    pub fn output(&self) -> &Path {
        &self.output
    }

    fn commands(&self, output: &Path, monitored: bool, cover: Option<&Path>) -> command::Commands {
        command::Job {
            input: &self.input,
            output,
            container: self.container,
            overwrite: true,
            monitored,
            video: &self.video,
            audio: &self.audio,
            cover,
        }
        .commands()
    }

    pub fn command_lines(&self) -> Vec<String> {
        let tools = &self.tools;
        let part = batch::part_path(&self.output);
        let cover = (!self.self_muxed())
            .then(|| batch::cover_path(&part))
            .filter(|_| self.thumbnail.is_some());
        let mut lines: Vec<String> = self
            .capture()
            .map(|c| c.command_line())
            .into_iter()
            .collect();
        lines.extend(match self.commands(&part, false, cover.as_deref()) {
            command::Commands::Single(args) => vec![tools.command_line(&args)],
            command::Commands::Piped {
                decode,
                mux: Some(mux),
                ..
            } => vec![
                format!("{} \\", tools.command_line(&decode)),
                "  | (built-in MJPEG encoder) \\".into(),
                format!("  | {}", tools.command_line(&mux)),
            ],
            command::Commands::Piped { decode, .. } => vec![
                format!("{} \\", tools.command_line(&decode)),
                "  | (built-in MJPEG encoder and mp4 muxer)".into(),
            ],
        });
        lines
    }

    /// Cover art is a nicety: a file that cannot give a frame is still
    /// converted, with a warning.
    fn cover(&self) -> Option<Vec<u8>> {
        let capture = self.capture()?;
        match capture.run() {
            Ok(jpeg) => Some(jpeg),
            Err(err) => {
                eprintln!("warning: {}: no thumbnail ({err:#})", self.input.display());
                None
            }
        }
    }

    pub fn run(&self, monitor: Option<&Monitor>) -> Result<Option<Stats>> {
        let part = batch::part_path(&self.output);
        let tools = &self.tools;
        let jpeg = self.cover();
        let mut cover_file = None;
        if let (Some(jpeg), false) = (&jpeg, self.self_muxed()) {
            let path = batch::cover_path(&part);
            std::fs::write(&path, jpeg).with_context(|| format!("writing {}", path.display()))?;
            cover_file = Some(path);
        }
        let result = match self.commands(&part, monitor.is_some(), cover_file.as_deref()) {
            command::Commands::Single(args) => match monitor {
                Some(monitor) => {
                    tools.run_monitored(&args, monitor, self.video.picture.rate.as_f64())
                }
                None => tools.run(&args),
            }
            .map(|()| None),
            command::Commands::Piped {
                decode,
                mux,
                settings,
            } => pipeline::run(
                &pipeline::Job {
                    tools,
                    decode: &decode,
                    mux: mux.as_deref(),
                    input: &self.input,
                    output: &part,
                    picture: &self.video.picture,
                    audio: &self.audio,
                    settings: &settings,
                    cover: jpeg.as_deref().filter(|_| self.self_muxed()),
                },
                monitor,
            )
            .map(Some),
        };
        if let Some(path) = &cover_file {
            let _ = std::fs::remove_file(path);
        }
        match result {
            Ok(stats) => std::fs::rename(&part, &self.output)
                .with_context(|| {
                    format!("renaming {} to {}", part.display(), self.output.display())
                })
                .map(|()| stats),
            Err(err) => {
                let _ = std::fs::remove_file(&part);
                Err(err)
            }
        }
    }
}
