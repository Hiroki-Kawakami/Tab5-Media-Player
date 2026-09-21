// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::ffi::OsString;
use std::path::Path;

use tab5conv_core::audio::AudioPlan;
use tab5conv_core::container::Container;
use tab5conv_core::video::mjpeg::Settings;
use tab5conv_core::video::{VideoCodec, VideoPlan};

use crate::args;

pub struct Job<'a> {
    pub input: &'a Path,
    pub output: &'a Path,
    pub container: Container,
    pub overwrite: bool,
    pub monitored: bool,
    pub video: &'a VideoPlan,
    pub audio: &'a AudioPlan,
}

pub enum Commands {
    Single(Vec<OsString>),
    Piped {
        decode: Vec<OsString>,
        mux: Option<Vec<OsString>>,
        settings: Settings,
    },
}

fn strings<const N: usize>(items: [&str; N]) -> impl Iterator<Item = OsString> {
    items.into_iter().map(OsString::from)
}

impl Job<'_> {
    pub fn commands(&self) -> Commands {
        let picture = &self.video.picture;
        let video_args = match &self.video.codec {
            VideoCodec::H264(params) => args::h264(picture, params),
            VideoCodec::Mpeg2(params) => args::mpeg2(picture, params),
            VideoCodec::Mjpeg(settings) => return self.piped(*settings),
        };
        let mut args = self.head();
        if self.monitored {
            args.extend(strings(["-progress", "pipe:1"]));
        }
        args.extend(strings(["-i"]));
        args.push(self.input.into());
        args.extend(["-map".into(), format!("0:{}", self.video.index).into()]);
        args.extend(video_args.into_iter().map(OsString::from));
        self.tail(&mut args, 0);
        Commands::Single(args)
    }

    fn piped(&self, settings: Settings) -> Commands {
        let picture = &self.video.picture;
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
        decode.extend(["-vf".into(), args::filter(picture).into()]);
        decode.extend(strings(["-pix_fmt", "yuv420p"]));

        if self.container == Container::Mp4 {
            decode.extend(strings(["-c:v", "rawvideo"]));
            if let Some(index) = self.audio.index() {
                decode.extend(["-map".into(), format!("0:{index}").into()]);
                decode.extend(args::audio(self.audio).into_iter().map(OsString::from));
            }
            decode.extend(strings(["-f", "matroska", "pipe:1"]));
            return Commands::Piped {
                decode,
                mux: None,
                settings,
            };
        }
        decode.extend(strings(["-f", "rawvideo", "pipe:1"]));

        let mut mux = self.head();
        if let Some(rotation) = picture.rotation.and_then(|r| r.display_rotation) {
            mux.extend(["-display_rotation".into(), rotation.to_string().into()]);
        }
        mux.extend(strings(["-f", "mjpeg", "-framerate"]));
        mux.push(picture.rate.to_string().into());
        mux.extend(strings(["-i", "pipe:0", "-i"]));
        mux.push(self.input.into());
        mux.extend(strings(["-map", "0:v", "-c:v", "copy"]));
        self.tail(&mut mux, 1);
        Commands::Piped {
            decode,
            mux: Some(mux),
            settings,
        }
    }

    fn head(&self) -> Vec<OsString> {
        strings([
            "-hide_banner",
            "-loglevel",
            "warning",
            if self.monitored { "-nostats" } else { "-stats" },
            if self.overwrite { "-y" } else { "-n" },
        ])
        .collect()
    }

    fn tail(&self, args: &mut Vec<OsString>, audio_input: usize) {
        if let Some(index) = self.audio.index() {
            args.extend(["-map".into(), format!("{audio_input}:{index}").into()]);
            args.extend(args::audio(self.audio).into_iter().map(OsString::from));
        }
        if self.container == Container::Mp4 {
            args.extend(strings(["-movflags", "+faststart"]));
        }
        args.push(self.output.into());
    }
}
