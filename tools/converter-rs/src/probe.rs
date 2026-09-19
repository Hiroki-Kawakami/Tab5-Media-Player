// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::HashMap;
use std::path::Path;

use anyhow::{Context, Result, bail};
use serde::Deserialize;

use crate::ffmpeg;
use crate::framerate::Rate;

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
    #[serde(default)]
    codec_name: String,
    profile: Option<String>,
    width: Option<u32>,
    height: Option<u32>,
    sample_aspect_ratio: Option<String>,
    avg_frame_rate: Option<String>,
    r_frame_rate: Option<String>,
    channels: Option<u32>,
    sample_rate: Option<String>,
    bit_rate: Option<String>,
    #[serde(default)]
    tags: HashMap<String, String>,
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
    pub video: Video,
    pub audio: Option<Audio>,
}

fn bit_rate(stream: &Stream) -> Option<u64> {
    let tagged = stream
        .tags
        .iter()
        .find(|(key, _)| *key == "BPS" || key.starts_with("BPS-"))
        .map(|(_, value)| value.as_str());
    [stream.bit_rate.as_deref(), tagged]
        .into_iter()
        .flatten()
        .find_map(|text| text.parse().ok().filter(|&b: &u64| b > 0))
}

fn frame_rate(text: &str) -> Option<Rate> {
    let (num, den) = text.split_once('/')?;
    Rate::new(num.parse().ok()?, den.parse().ok()?)
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
        .find_map(|r| r.as_deref().and_then(frame_rate));

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
            codec_name: s.codec_name.clone(),
            profile: s.profile.clone(),
            channels: s.channels.unwrap_or(2),
            sample_rate: s
                .sample_rate
                .as_deref()
                .and_then(|r| r.parse().ok())
                .unwrap_or(48000),
            bit_rate: bit_rate(s),
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
            {"index":2,"codec_type":"audio","codec_name":"aac","profile":"HE-AAC",
             "channels":6,"sample_rate":"44100"}
        ]}"#;
        let info = parse(json).unwrap();
        assert_eq!(info.video.index, 1);
        assert_eq!(info.video.display_width, 480.0);
        assert!((info.video.display_height - 853.33).abs() < 0.01);
        assert_eq!(info.video.fps, Rate::new(30000, 1001));
        let audio = info.audio.unwrap();
        assert_eq!(
            (audio.index, audio.channels, audio.sample_rate),
            (2, 6, 44100)
        );
        assert_eq!(audio.codec_name, "aac");
        assert_eq!(audio.profile.as_deref(), Some("HE-AAC"));
        assert_eq!(audio.bit_rate, None);
    }

    #[test]
    fn audio_bit_rate_from_stream_or_tags() {
        let rate = |audio: &str| {
            let json = format!(
                r#"{{"streams":[{{"index":0,"codec_type":"video","width":64,"height":64}},
                   {{"index":1,"codec_type":"audio",{audio}}}]}}"#
            );
            parse(&json).unwrap().audio.unwrap().bit_rate
        };
        assert_eq!(rate(r#""bit_rate":"128070""#), Some(128070));
        assert_eq!(
            rate(r#""tags":{"BPS":"160000","language":"eng"}"#),
            Some(160000)
        );
        assert_eq!(rate(r#""tags":{"BPS-eng":"96000"}"#), Some(96000));
        assert_eq!(
            rate(r#""bit_rate":"N/A","tags":{"BPS":"64000"}"#),
            Some(64000)
        );
        assert_eq!(rate(r#""tags":{"title":"x"}"#), None);
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
