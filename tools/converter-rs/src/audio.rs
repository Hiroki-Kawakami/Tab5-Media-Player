// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result, bail};

use crate::probe;
use crate::spec::{Spec, int_in, one_of, quantity};

pub const HELP: &str = "\
--audio <codec>[,key=value]...

aac    AAC-LC (default)
  keep=K              auto (default): copy an input that is already AAC-LC
                      with at most 2 channels at 16-48 kHz; none: always encode
  bitrate=R           e.g. 96k (default 160k)
  channels=N, samplerate=R
mp3    MP3 (libmp3lame)
  keep=K              auto (default): copy an input that is already MP3 with
                      at most 2 channels at 16-48 kHz; none: always encode
  bitrate=R           CBR; 32k-320k at 32k-48k (default 192k),
                      8k-160k at 16k-24k (default 160k)
  vbr=N               VBR quality 0-9, lower is better; replaces bitrate
  channels=N, samplerate=R
none   no audio

Encoding keys apply only when the audio is encoded.
  channels=N          1 or 2 (default: the input's, at most 2)
  samplerate=R        16k, 22.05k, 24k, 32k, 44.1k or 48k
                      (default: the input's, within 16k-48k)
";

const SAMPLE_RATES: [u32; 6] = [16000, 22050, 24000, 32000, 44100, 48000];
const MAX_CHANNELS: u32 = 2;
const MIN_SAMPLE_RATE: u32 = 16000;
const MAX_SAMPLE_RATE: u32 = 48000;
const AAC_DEFAULT_BITRATE: u64 = 160_000;
const MP3_DEFAULT_BITRATE: u64 = 192_000;
const MP3_LSF_DEFAULT_BITRATE: u64 = 160_000;
const MP3_MPEG1_KBPS: [u64; 14] = [
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
];
const MP3_LSF_KBPS: [u64; 14] = [8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160];
const MP3_MPEG1_MIN_RATE: u32 = 32000;

pub struct AudioPlan {
    pub index: Option<u32>,
    pub encoder: Option<&'static str>,
    pub label: String,
    pub args: Vec<String>,
}

pub enum AudioSpec {
    Encode { encoding: Encoding, keep: bool },
    None,
}

pub fn parse(text: &str) -> Result<AudioSpec> {
    let mut spec = Spec::parse(text)?;
    let codec = spec.codec.clone();
    let (audio, keys) = match codec.as_str() {
        "aac" | "mp3" => {
            let keep = spec
                .take("keep", |v| one_of(v, &["auto", "none"]))?
                .is_none_or(|keep| keep == "auto");
            let encoding = Encoding::take(&codec, &mut spec)?;
            let keys = [&["keep"][..], encoding.keys()].concat();
            (AudioSpec::Encode { encoding, keep }, keys)
        }
        "none" => (AudioSpec::None, Vec::new()),
        other => bail!("unknown audio codec '{other}' (codecs: aac, mp3, none)"),
    };
    spec.finish(&keys)?;
    Ok(audio)
}

impl AudioSpec {
    pub fn plan(&self, input: Option<&probe::Audio>) -> Result<AudioPlan> {
        let Some(input) = input else {
            return Ok(AudioPlan::none("input has no audio"));
        };
        match self {
            Self::None => Ok(AudioPlan::none("--audio none")),
            Self::Encode {
                encoding,
                keep: false,
            } => encoding.plan(input, None),
            Self::Encode {
                encoding,
                keep: true,
            } => match copy_blocker(input, encoding) {
                None => Ok(AudioPlan {
                    index: Some(input.index),
                    encoder: None,
                    label: format!("copy ({})", describe(input)),
                    args: vec!["-c:a".into(), "copy".into()],
                }),
                Some(reason) => encoding.plan(input, Some(reason)),
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

fn copy_blocker(input: &probe::Audio, encoding: &Encoding) -> Option<String> {
    let copyable = match encoding {
        Encoding::Aac(_) => input.codec_name == "aac" && input.profile.as_deref() == Some("LC"),
        Encoding::Mp3(_) => input.codec_name == "mp3",
    };
    if !copyable {
        Some(format!("input is {}", describe(input)))
    } else if input.channels > MAX_CHANNELS {
        Some(format!("input has {} channels", input.channels))
    } else if !(MIN_SAMPLE_RATE..=MAX_SAMPLE_RATE).contains(&input.sample_rate) {
        Some(format!("input is {} Hz", input.sample_rate))
    } else {
        None
    }
}

struct Format {
    channels: Option<u32>,
    sample_rate: Option<u32>,
}

impl Format {
    fn take(spec: &mut Spec) -> Result<Self> {
        let channels = spec.take("channels", |v| int_in(v, 1, MAX_CHANNELS))?;
        let sample_rate = spec.take("samplerate", |v| {
            u32::try_from(quantity(v)?)
                .ok()
                .filter(|rate| SAMPLE_RATES.contains(rate))
                .context("expected 16k, 22.05k, 24k, 32k, 44.1k or 48k")
        })?;
        Ok(Self {
            channels,
            sample_rate,
        })
    }

    fn resolve(&self, input: &probe::Audio) -> (u32, u32) {
        (
            self.channels
                .unwrap_or(input.channels.clamp(1, MAX_CHANNELS)),
            self.sample_rate
                .unwrap_or(input.sample_rate.clamp(MIN_SAMPLE_RATE, MAX_SAMPLE_RATE)),
        )
    }
}

pub enum Encoding {
    Aac(Aac),
    Mp3(Mp3),
}

impl Encoding {
    fn take(codec: &str, spec: &mut Spec) -> Result<Self> {
        Ok(match codec {
            "mp3" => Self::Mp3(Mp3::take(spec)?),
            _ => Self::Aac(Aac::take(spec)?),
        })
    }

    fn keys(&self) -> &'static [&'static str] {
        match self {
            Self::Aac(_) => &["bitrate", "channels", "samplerate"],
            Self::Mp3(_) => &["bitrate", "vbr", "channels", "samplerate"],
        }
    }

    fn plan(&self, input: &probe::Audio, reason: Option<String>) -> Result<AudioPlan> {
        let encoded = match self {
            Self::Aac(aac) => aac.encode(input)?,
            Self::Mp3(mp3) => mp3.encode(input)?,
        };
        let mut label = encoded.label;
        if let Some(reason) = reason {
            label.push_str(&format!(" ({reason})"));
        }
        Ok(AudioPlan {
            index: Some(input.index),
            encoder: Some(encoded.encoder),
            label,
            args: encoded.args,
        })
    }
}

struct Encoded {
    encoder: &'static str,
    label: String,
    args: Vec<String>,
}

fn kbps(bitrate: u64) -> String {
    format!("{} kbit/s", bitrate as f64 / 1000.0)
}

fn format_args(channels: u32, sample_rate: u32) -> [String; 4] {
    [
        "-ac".into(),
        channels.to_string(),
        "-ar".into(),
        sample_rate.to_string(),
    ]
}

pub struct Aac {
    bitrate: u64,
    format: Format,
}

impl Aac {
    fn take(spec: &mut Spec) -> Result<Self> {
        let bitrate = spec
            .take("bitrate", quantity)?
            .unwrap_or(AAC_DEFAULT_BITRATE);
        let format = Format::take(spec)?;
        Ok(Self { bitrate, format })
    }

    fn encode(&self, input: &probe::Audio) -> Result<Encoded> {
        let (channels, sample_rate) = self.format.resolve(input);
        let mut args: Vec<String> = vec![
            "-c:a".into(),
            "aac".into(),
            "-b:a".into(),
            self.bitrate.to_string(),
        ];
        args.extend(format_args(channels, sample_rate));
        Ok(Encoded {
            encoder: "aac",
            label: format!(
                "AAC-LC {}, {channels} ch, {sample_rate} Hz",
                kbps(self.bitrate)
            ),
            args,
        })
    }
}

enum Mp3Rate {
    Cbr(Option<u64>),
    Vbr(u32),
}

pub struct Mp3 {
    rate: Mp3Rate,
    format: Format,
}

impl Mp3 {
    fn take(spec: &mut Spec) -> Result<Self> {
        let bitrate = spec.take("bitrate", quantity)?;
        let vbr = spec.take("vbr", |v| int_in(v, 0, 9))?;
        let rate = match (bitrate, vbr) {
            (Some(_), Some(_)) => bail!("{}: bitrate and vbr cannot be combined", spec.codec),
            (_, Some(vbr)) => Mp3Rate::Vbr(vbr),
            (bitrate, None) => Mp3Rate::Cbr(bitrate),
        };
        let format = Format::take(spec)?;
        Ok(Self { rate, format })
    }

    fn encode(&self, input: &probe::Audio) -> Result<Encoded> {
        let (channels, sample_rate) = self.format.resolve(input);
        let mut args: Vec<String> = vec!["-c:a".into(), "libmp3lame".into()];
        let rate = match self.rate {
            Mp3Rate::Cbr(bitrate) => {
                let (valid, default): (&[u64], u64) = if sample_rate >= MP3_MPEG1_MIN_RATE {
                    (&MP3_MPEG1_KBPS, MP3_DEFAULT_BITRATE)
                } else {
                    (&MP3_LSF_KBPS, MP3_LSF_DEFAULT_BITRATE)
                };
                let bitrate = bitrate.unwrap_or(default);
                if bitrate % 1000 != 0 || !valid.contains(&(bitrate / 1000)) {
                    bail!(
                        "mp3: {} is not a valid bitrate at {sample_rate} Hz (valid: {})",
                        kbps(bitrate),
                        valid
                            .iter()
                            .map(|k| format!("{k}k"))
                            .collect::<Vec<_>>()
                            .join(", ")
                    );
                }
                args.extend(["-b:a".into(), bitrate.to_string()]);
                format!("{} CBR", kbps(bitrate))
            }
            Mp3Rate::Vbr(vbr) => {
                args.extend(["-q:a".into(), vbr.to_string()]);
                format!("VBR {vbr}")
            }
        };
        args.extend(format_args(channels, sample_rate));
        Ok(Encoded {
            encoder: "libmp3lame",
            label: format!("MP3 {rate}, {channels} ch, {sample_rate} Hz"),
            args,
        })
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

    fn lc() -> probe::Audio {
        input("aac", Some("LC"), 2, 48000)
    }

    fn plan(text: &str, input: &probe::Audio) -> AudioPlan {
        parse(text).unwrap().plan(Some(input)).unwrap()
    }

    fn has(plan: &AudioPlan, pair: [&str; 2]) -> bool {
        plan.args.windows(2).any(|w| w == pair)
    }

    #[test]
    fn keep_copies_the_same_codec() {
        let p = plan("aac", &lc());
        assert_eq!(p.args, ["-c:a", "copy"]);
        assert_eq!(p.label, "copy (AAC-LC)");
        assert!(p.encoder.is_none());
        let p = plan("mp3,bitrate=96k", &input("mp3", None, 1, 44100));
        assert_eq!(p.args, ["-c:a", "copy"]);
        assert_eq!(p.label, "copy (MP3)");
    }

    #[test]
    fn keep_encodes_a_different_codec() {
        let p = plan("aac", &input("mp3", None, 2, 44100));
        assert_eq!(p.encoder, Some("aac"));
        assert!(p.label.ends_with("(input is MP3)"), "{}", p.label);
        let p = plan("mp3", &lc());
        assert_eq!(p.encoder, Some("libmp3lame"));
        assert!(p.label.ends_with("(input is AAC-LC)"), "{}", p.label);
        let p = plan("aac,bitrate=96k", &input("opus", None, 2, 48000));
        assert!(has(&p, ["-b:a", "96000"]));
        assert!(p.label.ends_with("(input is opus)"), "{}", p.label);
    }

    #[test]
    fn keep_encodes_what_the_player_cannot_take() {
        let p = plan("aac", &input("aac", Some("HE-AAC"), 2, 48000));
        assert_eq!(p.encoder, Some("aac"));
        assert!(p.label.ends_with("(input is HE-AAC)"), "{}", p.label);
        let p = plan("aac", &input("aac", Some("LC"), 6, 48000));
        assert!(p.label.ends_with("(input has 6 channels)"), "{}", p.label);
        assert!(has(&p, ["-ac", "2"]));
        let p = plan("mp3", &input("mp3", None, 2, 96000));
        assert!(has(&p, ["-ar", "48000"]));
        let p = plan("mp3", &input("mp3", None, 1, 8000));
        assert!(p.label.ends_with("(input is 8000 Hz)"), "{}", p.label);
        assert!(has(&p, ["-ar", "16000"]));
    }

    #[test]
    fn keep_none_always_encodes() {
        let p = plan("aac,keep=none", &lc());
        assert_eq!(p.encoder, Some("aac"));
        assert!(!p.label.contains("("), "{}", p.label);
        let p = plan(
            "mp3,keep=none,vbr=2,channels=1",
            &input("mp3", None, 2, 44100),
        );
        assert!(has(&p, ["-q:a", "2"]));
        assert!(has(&p, ["-ac", "1"]));
    }

    #[test]
    fn aac_always_encodes() {
        let p = plan("aac,keep=none,channels=1,samplerate=44.1k", &lc());
        assert_eq!(p.encoder, Some("aac"));
        assert_eq!(
            p.args,
            ["-c:a", "aac", "-b:a", "160000", "-ac", "1", "-ar", "44100"]
        );
    }

    #[test]
    fn mp3_always_encodes() {
        let p = plan("mp3,keep=none", &input("mp3", None, 2, 44100));
        assert_eq!(p.encoder, Some("libmp3lame"));
        assert_eq!(
            p.args,
            [
                "-c:a",
                "libmp3lame",
                "-b:a",
                "192000",
                "-ac",
                "2",
                "-ar",
                "44100"
            ]
        );
        assert_eq!(p.label, "MP3 192 kbit/s CBR, 2 ch, 44100 Hz");
        let p = plan("mp3,samplerate=22.05k", &lc());
        assert!(has(&p, ["-b:a", "160000"]));
        let p = plan("mp3,keep=none", &input("mp3", None, 1, 16000));
        assert!(has(&p, ["-b:a", "160000"]));
        let p = plan("mp3,vbr=0,samplerate=22.05k", &lc());
        assert_eq!(
            p.args,
            [
                "-c:a",
                "libmp3lame",
                "-q:a",
                "0",
                "-ac",
                "2",
                "-ar",
                "22050"
            ]
        );
    }

    #[test]
    fn mp3_bitrate_must_suit_the_sample_rate() {
        let spec = |text| parse(text).unwrap().plan(Some(&lc()));
        assert!(spec("mp3,bitrate=320k").is_ok());
        assert!(spec("mp3,bitrate=144k").is_err());
        assert!(spec("mp3,bitrate=160k,samplerate=22.05k").is_ok());
        assert!(spec("mp3,bitrate=144k,samplerate=24k").is_ok());
        assert!(spec("mp3,bitrate=8k,samplerate=16k").is_ok());
        let err = spec("mp3,bitrate=320k,samplerate=22.05k").err().unwrap();
        assert!(
            err.to_string()
                .starts_with("mp3: 320 kbit/s is not a valid bitrate at 22050 Hz"),
            "{err}"
        );
        assert!(spec("mp3,bitrate=128500").is_err());
        let low = input("mp3", None, 1, 16000);
        assert!(parse("mp3").unwrap().plan(Some(&low)).is_ok());
        assert!(
            parse("mp3,keep=none,bitrate=192k")
                .unwrap()
                .plan(Some(&low))
                .is_err()
        );
    }

    #[test]
    fn none_and_missing_input() {
        let p = plan("none", &lc());
        assert!(p.index.is_none() && p.args.is_empty());
        let p = parse("aac").unwrap().plan(None).unwrap();
        assert_eq!(p.label, "none (input has no audio)");
    }

    #[test]
    fn errors() {
        assert!(parse("opus").is_err());
        assert!(parse("none,bitrate=96k").is_err());
        assert!(parse("aac,rate=48k").is_err());
        assert!(parse("aac,vbr=2").is_err());
        assert!(parse("aac,samplerate=96k").is_err());
        assert!(parse("aac,samplerate=44k").is_err());
        assert!(parse("aac,samplerate=12k").is_err());
        assert!(parse("mp3,samplerate=8k").is_err());
        assert!(parse("aac,channels=6").is_err());
        assert!(parse("mp3,bitrate=128k,vbr=2").is_err());
        assert!(parse("mp3,vbr=10").is_err());
        assert!(parse("mp3,fallback=aac").is_err());
        assert!(parse("auto").is_err());
        assert!(parse("aac,keep=always").is_err());
        let err = parse("aac,vbr=2").err().unwrap().to_string();
        assert!(
            err.contains("unknown key 'vbr'") && err.contains("keep"),
            "{err}"
        );
    }
}
