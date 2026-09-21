// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

pub mod audio;
pub mod color;
pub mod container;
pub mod framerate;
pub mod jpeg;
pub mod media;
pub mod mjpeg;
pub mod mpeg2;
pub mod num;
pub mod pcm;
pub mod preset;
pub mod ratecontrol;
pub mod resample;
pub mod size;
pub mod spec;
pub mod thumbnail;
pub mod video;
pub mod yuv;

use anyhow::Result;

use crate::spec::Spec;

pub struct Specs {
    pub preset: &'static str,
    pub video_text: String,
    pub audio_text: String,
    pub thumbnail_text: String,
    pub video: video::VideoSpec,
    pub audio: audio::AudioSpec,
    pub thumbnail: thumbnail::ThumbnailSpec,
}

impl Specs {
    pub fn resolve(
        preset: &str,
        video: Option<&str>,
        audio: Option<&str>,
        thumbnail: Option<&str>,
    ) -> Result<Self> {
        let preset = preset::find(preset)?;
        let video_spec = preset.video(video)?;
        let audio_spec = preset.audio(audio)?;
        let thumbnail_spec = Spec::parse(thumbnail.unwrap_or(thumbnail::DEFAULT))?;
        Ok(Self {
            preset: preset.name,
            video_text: video_spec.to_text(),
            audio_text: audio_spec.to_text(),
            thumbnail_text: thumbnail_spec.to_text(),
            video: video::from_spec(video_spec)?,
            audio: audio::from_spec(audio_spec)?,
            thumbnail: thumbnail::from_spec(thumbnail_spec)?,
        })
    }
}
