// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod args;
pub mod batch;
mod command;
mod pipeline;
mod probe;
mod process;

use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use tab5conv_core::Specs;
use tab5conv_core::audio::AudioPlan;
use tab5conv_core::container::Container;
use tab5conv_core::media::MediaInfo;
use tab5conv_core::video::VideoPlan;

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
        tools.require_encoders(&args::encoders(&video, &audio))?;
        Ok(Self {
            tools: tools.clone(),
            input: input.to_path_buf(),
            output: output.to_path_buf(),
            container,
            info,
            video,
            audio,
        })
    }

    pub fn output(&self) -> &Path {
        &self.output
    }

    fn commands(&self, output: &Path, monitored: bool) -> command::Commands {
        command::Job {
            input: &self.input,
            output,
            container: self.container,
            overwrite: true,
            monitored,
            video: &self.video,
            audio: &self.audio,
        }
        .commands()
    }

    pub fn command_lines(&self) -> Vec<String> {
        let tools = &self.tools;
        match self.commands(&batch::part_path(&self.output), false) {
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
        }
    }

    pub fn run(&self, monitor: Option<&Monitor>) -> Result<Option<Stats>> {
        let part = batch::part_path(&self.output);
        let tools = &self.tools;
        let result = match self.commands(&part, monitor.is_some()) {
            command::Commands::Single(args) => match monitor {
                Some(monitor) => tools.run_monitored(&args, monitor),
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
                },
                monitor,
            )
            .map(Some),
        };
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
