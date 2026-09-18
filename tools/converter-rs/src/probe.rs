// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::path::Path;

use anyhow::{Context, Result, bail};
use serde::Deserialize;

use crate::ffmpeg;

#[derive(Deserialize)]
struct ProbeOutput {
    #[serde(default)]
    streams: Vec<Stream>,
}

#[derive(Deserialize)]
struct Stream {
    index: u32,
    #[serde(default)]
    codec_type: String,
    width: Option<u32>,
    height: Option<u32>,
    sample_aspect_ratio: Option<String>,
    avg_frame_rate: Option<String>,
    r_frame_rate: Option<String>,
    channels: Option<u32>,
    sample_rate: Option<String>,
    #[serde(default)]
    disposition: Disposition,
    #[serde(default)]
    side_data_list: Vec<SideData>,
}

#[derive(Deserialize, Default)]
struct Disposition {
    #[serde(default)]
    attached_pic: u8,
}

#[derive(Deserialize)]
struct SideData {
    rotation: Option<f64>,
}

pub struct Video {
    pub index: u32,
    pub display_width: f64,
    pub display_height: f64,
    pub fps: Option<f64>,
}

pub struct Audio {
    pub index: u32,
    pub channels: u32,
    pub sample_rate: u32,
}

pub struct MediaInfo {
    pub video: Video,
    pub audio: Option<Audio>,
}

fn ratio(text: &str, separator: char) -> Option<f64> {
    let (num, den) = text.split_once(separator)?;
    let num: f64 = num.parse().ok()?;
    let den: f64 = den.parse().ok()?;
    (num > 0.0 && den > 0.0).then(|| num / den)
}

fn video_info(stream: &Stream) -> Result<Video> {
    let (Some(width), Some(height)) = (stream.width, stream.height) else {
        bail!("video stream {} has no size", stream.index);
    };
    let sar = stream
        .sample_aspect_ratio
        .as_deref()
        .and_then(|s| ratio(s, ':'))
        .unwrap_or(1.0);
    let mut display_width = width as f64 * sar;
    let mut display_height = height as f64;

    let rotation = stream
        .side_data_list
        .iter()
        .find_map(|d| d.rotation)
        .unwrap_or(0.0);
    let quarter_turns = (rotation / 90.0).round() as i64;
    if quarter_turns.rem_euclid(2) == 1 {
        std::mem::swap(&mut display_width, &mut display_height);
    }

    let fps = [&stream.avg_frame_rate, &stream.r_frame_rate]
        .into_iter()
        .find_map(|r| r.as_deref().and_then(|s| ratio(s, '/')));

    Ok(Video {
        index: stream.index,
        display_width,
        display_height,
        fps,
    })
}

fn parse(json: &str) -> Result<MediaInfo> {
    let output: ProbeOutput = serde_json::from_str(json).context("unexpected ffprobe output")?;

    let video = output
        .streams
        .iter()
        .find(|s| s.codec_type == "video" && s.disposition.attached_pic == 0)
        .context("no video stream")?;
    let audio = output
        .streams
        .iter()
        .find(|s| s.codec_type == "audio")
        .map(|s| Audio {
            index: s.index,
            channels: s.channels.unwrap_or(2),
            sample_rate: s
                .sample_rate
                .as_deref()
                .and_then(|r| r.parse().ok())
                .unwrap_or(48000),
        });

    Ok(MediaInfo {
        video: video_info(video)?,
        audio,
    })
}

pub fn probe(input: &Path) -> Result<MediaInfo> {
    let json = ffmpeg::probe_json(input)?;
    parse(&json).with_context(|| format!("cannot use {}", input.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotated_anamorphic_video_with_cover_art() {
        let json = r#"{"streams":[
            {"index":0,"codec_type":"video","width":600,"height":600,
             "disposition":{"attached_pic":1}},
            {"index":1,"codec_type":"video","width":720,"height":480,
             "sample_aspect_ratio":"32:27","avg_frame_rate":"30000/1001",
             "side_data_list":[{"side_data_type":"Display Matrix","rotation":-90}]},
            {"index":2,"codec_type":"audio","channels":6,"sample_rate":"44100"}
        ]}"#;
        let info = parse(json).unwrap();
        assert_eq!(info.video.index, 1);
        assert_eq!(info.video.display_width, 480.0);
        assert!((info.video.display_height - 853.33).abs() < 0.01);
        assert!((info.video.fps.unwrap() - 29.97).abs() < 0.01);
        let audio = info.audio.unwrap();
        assert_eq!(
            (audio.index, audio.channels, audio.sample_rate),
            (2, 6, 44100)
        );
    }

    #[test]
    fn unknown_frame_rate_and_no_audio() {
        let json = r#"{"streams":[
            {"index":0,"codec_type":"video","width":640,"height":360,
             "sample_aspect_ratio":"0:1","avg_frame_rate":"0/0","r_frame_rate":"0/0"}
        ]}"#;
        let info = parse(json).unwrap();
        assert_eq!(info.video.display_width, 640.0);
        assert!(info.video.fps.is_none());
        assert!(info.audio.is_none());
    }

    #[test]
    fn no_video() {
        let json = r#"{"streams":[{"index":0,"codec_type":"audio"}]}"#;
        assert!(parse(json).is_err());
    }
}
