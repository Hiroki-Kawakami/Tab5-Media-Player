// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result, bail};

use crate::probe;
use crate::spec::{Spec, int_in, quantity};

pub const HELP: &str = "\
--audio <codec>[,key=value]...

auto   copy the input audio when it is AAC-LC or MP3 with at most 2 channels
       and 48 kHz, otherwise encode it as aac (default)
aac    encode AAC-LC
none   no audio

keys for auto and aac (auto uses them only when it encodes):
  bitrate=R           e.g. 96k (default 128k)
  channels=N          1 or 2 (default: the input's, at most 2)
  samplerate=R        8k, 11.025k, 12k, 16k, 22.05k, 24k, 32k, 44.1k or 48k
                      (default: the input's, at most 48k)
";

const AAC_KEYS: [&str; 3] = ["bitrate", "channels", "samplerate"];
const SAMPLE_RATES: [u32; 9] = [8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000];
const MAX_CHANNELS: u32 = 2;
const MAX_SAMPLE_RATE: u32 = 48000;
const DEFAULT_BITRATE: u64 = 128_000;

pub struct AudioPlan {
    pub index: Option<u32>,
    pub encoder: Option<&'static str>,
    pub label: String,
    pub args: Vec<String>,
}

pub enum AudioSpec {
    Auto(Aac),
    Aac(Aac),
    None,
}

pub fn parse(text: &str) -> Result<AudioSpec> {
    let mut spec = Spec::parse(text)?;
    let audio = match spec.codec.clone().as_str() {
        "auto" => AudioSpec::Auto(Aac::take(&mut spec)?),
        "aac" => AudioSpec::Aac(Aac::take(&mut spec)?),
        "none" => AudioSpec::None,
        other => bail!("unknown audio codec '{other}' (codecs: auto, aac, none)"),
    };
    let keys: &[&str] = match audio {
        AudioSpec::None => &[],
        _ => &AAC_KEYS,
    };
    spec.finish(keys)?;
    Ok(audio)
}

impl AudioSpec {
    pub fn plan(&self, input: Option<&probe::Audio>) -> AudioPlan {
        let Some(input) = input else {
            return AudioPlan::none("input has no audio");
        };
        match self {
            Self::None => AudioPlan::none("--audio none"),
            Self::Aac(aac) => aac.plan(input, None),
            Self::Auto(aac) => match copy_blocker(input) {
                None => AudioPlan {
                    index: Some(input.index),
                    encoder: None,
                    label: format!("copy ({})", describe(input)),
                    args: vec!["-c:a".into(), "copy".into()],
                },
                Some(reason) => aac.plan(input, Some(reason)),
            },
        }
    }
}

impl AudioPlan {
    fn none(reason: &str) -> Self {
        Self {
            index: None,
            encoder: None,
            label: format!("none ({reason})"),
            args: Vec::new(),
        }
    }
}

fn describe(input: &probe::Audio) -> String {
    match (input.codec_name.as_str(), input.profile.as_deref()) {
        ("aac", Some("LC")) => "AAC-LC".into(),
        ("aac", Some(p)) if p.starts_with("HE") => p.into(),
        ("aac", Some(p)) => format!("AAC {p}"),
        ("aac", None) => "AAC".into(),
        ("mp3", _) => "MP3".into(),
        (codec, _) => codec.into(),
    }
}

fn copy_blocker(input: &probe::Audio) -> Option<String> {
    let copyable = match input.codec_name.as_str() {
        "aac" => input.profile.as_deref() == Some("LC"),
        "mp3" => true,
        _ => false,
    };
    if !copyable {
        Some(format!("input is {}", describe(input)))
    } else if input.channels > MAX_CHANNELS {
        Some(format!("input has {} channels", input.channels))
    } else if input.sample_rate > MAX_SAMPLE_RATE {
        Some(format!("input is {} Hz", input.sample_rate))
    } else {
        None
    }
}

pub struct Aac {
    bitrate: u64,
    channels: Option<u32>,
    sample_rate: Option<u32>,
}

impl Aac {
    fn take(spec: &mut Spec) -> Result<Self> {
        let bitrate = spec.take("bitrate", quantity)?.unwrap_or(DEFAULT_BITRATE);
        let channels = spec.take("channels", |v| int_in(v, 1, MAX_CHANNELS))?;
        let sample_rate = spec.take("samplerate", |v| {
            u32::try_from(quantity(v)?)
                .ok()
                .filter(|rate| SAMPLE_RATES.contains(rate))
                .context("expected 8k, 11.025k, 12k, 16k, 22.05k, 24k, 32k, 44.1k or 48k")
        })?;
        Ok(Self {
            bitrate,
            channels,
            sample_rate,
        })
    }

    fn plan(&self, input: &probe::Audio, reason: Option<String>) -> AudioPlan {
        let channels = self
            .channels
            .unwrap_or(input.channels.clamp(1, MAX_CHANNELS));
        let sample_rate = self
            .sample_rate
            .unwrap_or(input.sample_rate.min(MAX_SAMPLE_RATE));
        let mut label = format!(
            "AAC-LC {} kbit/s, {channels} ch, {sample_rate} Hz",
            self.bitrate as f64 / 1000.0
        );
        if let Some(reason) = reason {
            label.push_str(&format!(" ({reason})"));
        }
        AudioPlan {
            index: Some(input.index),
            encoder: Some("aac"),
            label,
            args: vec![
                "-c:a".into(),
                "aac".into(),
                "-b:a".into(),
                self.bitrate.to_string(),
                "-ac".into(),
                channels.to_string(),
                "-ar".into(),
                sample_rate.to_string(),
            ],
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(codec: &str, profile: Option<&str>, channels: u32, rate: u32) -> probe::Audio {
        probe::Audio {
            index: 1,
            codec_name: codec.into(),
            profile: profile.map(String::from),
            channels,
            sample_rate: rate,
        }
    }

    fn plan(text: &str, input: &probe::Audio) -> AudioPlan {
        parse(text).unwrap().plan(Some(input))
    }

    #[test]
    fn auto_copies_aac_lc_and_mp3() {
        let p = plan("auto", &input("aac", Some("LC"), 2, 48000));
        assert_eq!(p.args, ["-c:a", "copy"]);
        assert_eq!(p.label, "copy (AAC-LC)");
        assert!(p.encoder.is_none());
        let p = plan("auto,bitrate=96k", &input("mp3", None, 1, 44100));
        assert_eq!(p.args, ["-c:a", "copy"]);
    }

    #[test]
    fn auto_encodes_what_it_cannot_copy() {
        let p = plan("auto", &input("aac", Some("HE-AAC"), 2, 48000));
        assert_eq!(p.encoder, Some("aac"));
        assert!(p.label.ends_with("(input is HE-AAC)"), "{}", p.label);
        let p = plan("auto", &input("aac", Some("LC"), 6, 48000));
        assert!(p.label.ends_with("(input has 6 channels)"), "{}", p.label);
        assert!(p.args.windows(2).any(|w| w == ["-ac", "2"]));
        let p = plan("auto", &input("mp3", None, 2, 96000));
        assert!(p.args.windows(2).any(|w| w == ["-ar", "48000"]));
        let p = plan("auto,bitrate=96k", &input("opus", None, 2, 48000));
        assert!(p.args.windows(2).any(|w| w == ["-b:a", "96000"]));
        assert!(p.label.ends_with("(input is opus)"), "{}", p.label);
    }

    #[test]
    fn aac_always_encodes() {
        let p = plan(
            "aac,channels=1,samplerate=44.1k",
            &input("aac", Some("LC"), 2, 48000),
        );
        assert_eq!(p.encoder, Some("aac"));
        assert_eq!(
            p.args,
            ["-c:a", "aac", "-b:a", "128000", "-ac", "1", "-ar", "44100"]
        );
    }

    #[test]
    fn none_and_missing_input() {
        let p = plan("none", &input("aac", Some("LC"), 2, 48000));
        assert!(p.index.is_none() && p.args.is_empty());
        let p = parse("auto").unwrap().plan(None);
        assert_eq!(p.label, "none (input has no audio)");
    }

    #[test]
    fn errors() {
        assert!(parse("opus").is_err());
        assert!(parse("none,bitrate=96k").is_err());
        assert!(parse("aac,rate=48k").is_err());
        assert!(parse("aac,samplerate=96k").is_err());
        assert!(parse("aac,samplerate=44k").is_err());
        assert!(parse("aac,channels=6").is_err());
    }
}
