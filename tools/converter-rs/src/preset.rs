// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result};

use crate::spec::Spec;

pub struct Preset {
    pub name: &'static str,
    video: &'static str,
    audio: &'static str,
}

const PRESETS: [Preset; 4] = [
    Preset {
        name: "tiny",
        video: "h264,long=640,short=360,maxfps=30",
        audio: "aac,bitrate=128k",
    },
    Preset {
        name: "small",
        video: "mpeg2,long=640,short=360,maxfps=30",
        audio: "aac,bitrate=128k",
    },
    Preset {
        name: "default",
        video: "mjpeg",
        audio: "aac",
    },
    Preset {
        name: "quality",
        video: "mjpeg,long=1280,short=720,maxfps=60,quality=80,bitrate=40M",
        audio: "aac",
    },
];

pub const DEFAULT: &str = "default";

pub fn help() -> String {
    let mut text = String::from(
        "--preset <name>\n\n\
         A preset sets --video and --audio. An explicit --video or --audio with the\n\
         same codec adds to the preset's keys and wins on conflicts; one with another\n\
         codec replaces the preset's side.\n\n",
    );
    for preset in &PRESETS {
        text.push_str(&format!(
            "{:<8} --video \"{}\" --audio \"{}\"\n",
            preset.name, preset.video, preset.audio
        ));
    }
    text
}

pub fn find(name: &str) -> Result<&'static Preset> {
    PRESETS.iter().find(|p| p.name == name).with_context(|| {
        let names: Vec<&str> = PRESETS.iter().map(|p| p.name).collect();
        format!("unknown preset '{name}' (presets: {})", names.join(", "))
    })
}

fn combine(base: &str, explicit: Option<&str>) -> Result<Spec> {
    let base = Spec::parse(base)?;
    Ok(match explicit {
        Some(text) => base.merged(Spec::parse(text)?),
        None => base,
    })
}

impl Preset {
    pub fn video(&self, explicit: Option<&str>) -> Result<Spec> {
        combine(self.video, explicit)
    }

    pub fn audio(&self, explicit: Option<&str>) -> Result<Spec> {
        combine(self.audio, explicit)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::framerate::Rate;
    use crate::{audio, probe, video};

    #[test]
    fn every_preset_plans() {
        let source = probe::Video {
            index: 0,
            display_width: 1920.0,
            display_height: 1080.0,
            fps: Rate::new(60, 1),
        };
        let input = probe::Audio {
            index: 1,
            codec_name: "aac".into(),
            profile: Some("LC".into()),
            channels: 2,
            sample_rate: 48000,
            bit_rate: Some(320_000),
        };
        for preset in &PRESETS {
            let v = video::from_spec(preset.video(None).unwrap()).unwrap();
            v.plan(&source).unwrap();
            let a = audio::from_spec(preset.audio(None).unwrap()).unwrap();
            a.plan(Some(&input)).unwrap();
        }
    }

    #[test]
    fn explicit_specs_merge_or_replace() {
        let tiny = find("tiny").unwrap();
        assert_eq!(
            tiny.video(Some("h264,crf=20,maxfps=24")).unwrap().to_text(),
            "h264,long=640,short=360,crf=20,maxfps=24"
        );
        assert_eq!(tiny.video(Some("mjpeg")).unwrap().to_text(), "mjpeg");
        assert_eq!(tiny.audio(Some("mp3")).unwrap().to_text(), "mp3");
        assert_eq!(tiny.audio(Some("none")).unwrap().to_text(), "none");
        assert_eq!(
            find("default").unwrap().video(None).unwrap().to_text(),
            "mjpeg"
        );
    }

    #[test]
    fn unknown_preset() {
        let err = find("huge").err().unwrap().to_string();
        assert!(err.contains("tiny, small, default, quality"), "{err}");
    }
}
