// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use crate::audio::{self, AudioAction, AudioPlan};
use crate::framerate::{self, Rate};
use crate::media::MediaInfo;
use crate::spec::quantity_text;
use crate::video::{VideoCodec, VideoPlan, h264, mpeg2};

fn codec_name(name: &str) -> String {
    match name {
        "h264" => "H.264".into(),
        "hevc" => "HEVC".into(),
        "mpeg1video" => "MPEG-1".into(),
        "mpeg2video" => "MPEG-2".into(),
        "mpeg4" => "MPEG-4".into(),
        "prores" => "ProRes".into(),
        "rawvideo" => "Raw".into(),
        other => other.to_uppercase(),
    }
}

fn fps(rate: Rate) -> String {
    format!("{} fps", framerate::describe(rate.as_f64()))
}

fn duration(seconds: f64) -> String {
    let total = seconds.floor() as u64;
    let (h, m, s) = (total / 3600, total / 60 % 60, total % 60);
    if h > 0 {
        format!("{h}:{m:02}:{s:02}")
    } else {
        format!("{m}:{s:02}")
    }
}

pub fn source(info: &MediaInfo) -> Vec<String> {
    let video = &info.video;
    let mut parts = vec![format!(
        "{}×{} {}",
        video.display_width.round(),
        video.display_height.round(),
        codec_name(&video.codec_name)
    )];
    parts.extend(video.fps.map(fps));
    parts.extend(info.duration.map(duration));
    parts
}

pub fn output(info: &MediaInfo, video: &VideoPlan, audio: &AudioPlan) -> Vec<String> {
    let rate = match &video.codec {
        VideoCodec::Mjpeg(s) => format!("MJPEG q{}", s.quality),
        VideoCodec::H264(p) => match p.rate {
            h264::Rate::Crf(crf) => format!("H.264 crf {crf}"),
            h264::Rate::Bitrate(b) => format!("H.264 {}", quantity_text(b)),
        },
        VideoCodec::Mpeg2(p) => match p.rate {
            mpeg2::Rate::Qscale(q) => format!("MPEG-2 q{q}"),
            mpeg2::Rate::Bitrate(b) => format!("MPEG-2 {}", quantity_text(b)),
        },
    };
    let p = &video.picture;
    let sound = match (audio.action, info.audio.as_ref()) {
        (AudioAction::Copy { .. }, Some(input)) => format!("{} copied", audio::describe(input)),
        (AudioAction::Encode { codec, .. }, _) => match codec {
            audio::Codec::Aac { bitrate } => format!("AAC {}", quantity_text(bitrate)),
            audio::Codec::Mp3Cbr { bitrate } => format!("MP3 {}", quantity_text(bitrate)),
            audio::Codec::Mp3Vbr { quality } => format!("MP3 VBR {quality}"),
        },
        _ => "no audio".into(),
    };
    vec![
        format!("{}×{} {rate}", p.resize.width, p.resize.height),
        fps(p.rate),
        sound,
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Specs, media};

    fn info(audio: Option<media::Audio>) -> MediaInfo {
        MediaInfo {
            duration: Some(3725.5),
            video: media::Video {
                index: 0,
                codec_name: "hevc".into(),
                display_width: 1920.0,
                display_height: 1080.0,
                fps: Rate::new(30000, 1001),
            },
            audio,
        }
    }

    fn aac(bit_rate: u64) -> media::Audio {
        media::Audio {
            index: 1,
            codec_name: "aac".into(),
            profile: Some("LC".into()),
            channels: 2,
            sample_rate: 48000,
            bit_rate: Some(bit_rate),
        }
    }

    fn output_for(
        preset: &str,
        video: Option<&str>,
        audio: Option<&str>,
        info: &MediaInfo,
    ) -> Vec<String> {
        let specs = Specs::resolve(preset, video, audio, None).unwrap();
        let v = specs.video.plan(&info.video).unwrap();
        let a = specs.audio.plan(info.audio.as_ref()).unwrap();
        output(info, &v, &a)
    }

    #[test]
    fn source_parts() {
        assert_eq!(
            source(&info(None)),
            ["1920×1080 HEVC", "29.97 fps", "1:02:05"]
        );
        let short = MediaInfo {
            duration: Some(65.9),
            ..info(None)
        };
        assert_eq!(source(&short)[2], "1:05");
    }

    #[test]
    fn output_parts() {
        let i = info(Some(aac(128_000)));
        assert_eq!(
            output_for("default", None, None, &i),
            ["1280×720 MJPEG q75", "29.97 fps", "AAC-LC copied"]
        );
        assert_eq!(
            output_for("tiny", None, None, &info(Some(aac(192_000)))),
            ["640×360 H.264 crf 32", "29.97 fps", "AAC 128k"]
        );
        assert_eq!(
            output_for(
                "small",
                Some("mpeg2,bitrate=1.5M,maxfps=24"),
                Some("mp3,vbr=2"),
                &i
            ),
            ["640×360 MPEG-2 1.5M", "24 fps", "MP3 VBR 2"]
        );
        assert_eq!(
            output_for("default", None, None, &info(None))[2],
            "no audio"
        );
    }
}
