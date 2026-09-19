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

pub use pipeline::Stats;

pub struct Conversion {
    input: PathBuf,
    output: PathBuf,
    container: Container,
    pub info: MediaInfo,
    pub video: VideoPlan,
    pub audio: AudioPlan,
}

impl Conversion {
    pub fn prepare(
        input: &Path,
        output: &Path,
        container: Container,
        specs: &Specs,
    ) -> Result<Self> {
        let info = probe::probe(input)?;
        let video = specs.video.plan(&info.video)?;
        let audio = specs.audio.plan(info.audio.as_ref())?;
        process::require_encoders(&args::encoders(&video, &audio))?;
        Ok(Self {
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

    fn commands(&self, output: &Path) -> command::Commands {
        command::Job {
            input: &self.input,
            output,
            container: self.container,
            overwrite: true,
            video: &self.video,
            audio: &self.audio,
        }
        .commands()
    }

    pub fn command_lines(&self) -> Vec<String> {
        match self.commands(&batch::part_path(&self.output)) {
            command::Commands::Single(args) => vec![process::command_line(&args)],
            command::Commands::Piped { decode, mux, .. } => vec![
                format!("{} \\", process::command_line(&decode)),
                "  | (built-in MJPEG encoder) \\".into(),
                format!("  | {}", process::command_line(&mux)),
            ],
        }
    }

    pub fn run(&self) -> Result<Option<Stats>> {
        let part = batch::part_path(&self.output);
        let result = match self.commands(&part) {
            command::Commands::Single(args) => process::run(&args).map(|()| None),
            command::Commands::Piped {
                decode,
                mux,
                settings,
            } => pipeline::run(&decode, &mux, &self.video.picture, &settings).map(Some),
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
