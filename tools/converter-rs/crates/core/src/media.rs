// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use crate::framerate::Rate;

pub struct Video {
    pub index: u32,
    pub display_width: f64,
    pub display_height: f64,
    pub fps: Option<Rate>,
}

pub struct Audio {
    pub index: u32,
    pub codec_name: String,
    pub profile: Option<String>,
    pub channels: u32,
    pub sample_rate: u32,
    pub bit_rate: Option<u64>,
}

pub struct MediaInfo {
    pub duration: Option<f64>,
    pub video: Video,
    pub audio: Option<Audio>,
}
