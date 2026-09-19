// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result, bail};

use crate::media;
use crate::spec::{Spec, int_in, one_of, quantity};

pub const HELP: &str = "\
--audio <codec>[,key=value]...

aac    AAC-LC (default)
  keep=K              auto (default): copy an input that is already AAC-LC
                      with at most 2 channels at 16-48 kHz and no more than
                      bitrate (5% slack); none: always encode
  bitrate=R           e.g. 96k (default 160k)
  channels=N, samplerate=R
mp3    MP3 (libmp3lame)
  keep=K              auto (default): copy an input that is already MP3 with
                      at most 2 channels at 16-48 kHz and no more than
                      bitrate (5% slack; any bitrate with vbr); none: always
                      encode
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

pub const SAMPLE_RATES: [u32; 6] = [16000, 22050, 24000, 32000, 44100, 48000];
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
const BITRATE_TOLERANCE: f64 = 1.05;

pub struct AudioPlan {
    pub label: String,
    pub action: AudioAction,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum AudioAction {
    None,
    Copy {
        index: u32,
    },
    Encode {
        index: u32,
        codec: Codec,
        channels: u32,
        sample_rate: u32,
    },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Codec {
    Aac { bitrate: u64 },
    Mp3Cbr { bitrate: u64 },
    Mp3Vbr { quality: u32 },
}

pub enum AudioSpec {
    Encode { encoding: Encoding, keep: bool },
    None,
}

#[cfg(test)]
pub fn parse(text: &str) -> Result<AudioSpec> {
    from_spec(Spec::parse(text)?)
}

pub fn from_spec(mut spec: Spec) -> Result<AudioSpec> {
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
    pub fn plan(&self, input: Option<&media::Audio>) -> Result<AudioPlan> {
        self.plan_for(input, &SAMPLE_RATES)
    }

    pub fn plan_for(&self, input: Option<&media::Audio>, aac_rates: &[u32]) -> Result<AudioPlan> {
        let Some(input) = input else {
            return Ok(AudioPlan::none("input has no audio"));
        };
        match self {
            Self::None => Ok(AudioPlan::none("--audio none")),
            Self::Encode {
                encoding,
                keep: false,
            } => encoding.plan(input, None, aac_rates),
            Self::Encode {
                encoding,
                keep: true,
            } => match copy_blocker(input, encoding) {
                None => Ok(AudioPlan {
                    label: format!("copy ({})", describe(input)),
                    action: AudioAction::Copy { index: input.index },
                }),
                Some(reason) => encoding.plan(input, Some(reason), aac_rates),
            },
        }
    }
}

impl AudioPlan {
    fn none(reason: &str) -> Self {
        Self {
            label: format!("none ({reason})"),
            action: AudioAction::None,
        }
    }

    pub fn index(&self) -> Option<u32> {
        match self.action {
            AudioAction::None => None,
            AudioAction::Copy { index } | AudioAction::Encode { index, .. } => Some(index),
        }
    }
}

fn describe(input: &media::Audio) -> String {
    match (input.codec_name.as_str(), input.profile.as_deref()) {
        ("aac", Some("LC")) => "AAC-LC".into(),
        ("aac", Some(p)) if p.starts_with("HE") => p.into(),
        ("aac", Some(p)) => format!("AAC {p}"),
        ("aac", None) => "AAC".into(),
        ("mp3", _) => "MP3".into(),
        (codec, _) => codec.into(),
    }
}

fn copy_blocker(input: &media::Audio, encoding: &Encoding) -> Option<String> {
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
        let target = encoding.target_bitrate(input)?;
        match input.bit_rate {
            None => Some("input bitrate unknown".into()),
            Some(rate) if rate as f64 > target as f64 * BITRATE_TOLERANCE => Some(format!(
                "input is {} kbit/s",
                (rate as f64 / 1000.0).round()
            )),
            Some(_) => None,
        }
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

    fn resolve(&self, input: &media::Audio) -> (u32, u32) {
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

    fn target_bitrate(&self, input: &media::Audio) -> Option<u64> {
        match self {
            Self::Aac(aac) => Some(aac.bitrate),
            Self::Mp3(mp3) => match mp3.rate {
                Mp3Rate::Cbr(bitrate) => {
                    let (_, sample_rate) = mp3.format.resolve(input);
                    Some(bitrate.unwrap_or(mp3_default_bitrate(sample_rate)))
                }
                Mp3Rate::Vbr(_) => None,
            },
        }
    }

    fn plan(
        &self,
        input: &media::Audio,
        reason: Option<String>,
        aac_rates: &[u32],
    ) -> Result<AudioPlan> {
        let encoded = match self {
            Self::Aac(aac) => aac.encode(input, aac_rates)?,
            Self::Mp3(mp3) => mp3.encode(input)?,
        };
        let mut label = encoded.label;
        if let Some(reason) = reason {
            label.push_str(&format!(" ({reason})"));
        }
        Ok(AudioPlan {
            label,
            action: AudioAction::Encode {
                index: input.index,
                codec: encoded.codec,
                channels: encoded.channels,
                sample_rate: encoded.sample_rate,
            },
        })
    }
}

struct Encoded {
    label: String,
    codec: Codec,
    channels: u32,
    sample_rate: u32,
}

fn kbps(bitrate: u64) -> String {
    format!("{} kbit/s", bitrate as f64 / 1000.0)
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

    fn encode(&self, input: &media::Audio, rates: &[u32]) -> Result<Encoded> {
        let (channels, mut sample_rate) = self.format.resolve(input);
        if !rates.contains(&sample_rate) {
            let supported = || {
                rates
                    .iter()
                    .map(|r| format!("{}k", *r as f64 / 1000.0))
                    .collect::<Vec<_>>()
                    .join(", ")
            };
            if rates.is_empty() {
                bail!("aac: AAC cannot be encoded here");
            }
            if self.format.sample_rate.is_some() {
                bail!(
                    "aac: {sample_rate} Hz cannot be encoded here (supported: {})",
                    supported()
                );
            }
            sample_rate = rates
                .iter()
                .copied()
                .find(|&r| r >= sample_rate)
                .unwrap_or(*rates.last().expect("not empty"));
        }
        Ok(Encoded {
            label: format!(
                "AAC-LC {}, {channels} ch, {sample_rate} Hz",
                kbps(self.bitrate)
            ),
            codec: Codec::Aac {
                bitrate: self.bitrate,
            },
            channels,
            sample_rate,
        })
    }
}

fn mp3_default_bitrate(sample_rate: u32) -> u64 {
    if sample_rate >= MP3_MPEG1_MIN_RATE {
        MP3_DEFAULT_BITRATE
    } else {
        MP3_LSF_DEFAULT_BITRATE
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

    fn encode(&self, input: &media::Audio) -> Result<Encoded> {
        let (channels, sample_rate) = self.format.resolve(input);
        let (codec, rate) = match self.rate {
            Mp3Rate::Cbr(bitrate) => {
                let valid: &[u64] = if sample_rate >= MP3_MPEG1_MIN_RATE {
                    &MP3_MPEG1_KBPS
                } else {
                    &MP3_LSF_KBPS
                };
                let bitrate = bitrate.unwrap_or(mp3_default_bitrate(sample_rate));
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
                (Codec::Mp3Cbr { bitrate }, format!("{} CBR", kbps(bitrate)))
            }
            Mp3Rate::Vbr(vbr) => (Codec::Mp3Vbr { quality: vbr }, format!("VBR {vbr}")),
        };
        Ok(Encoded {
            label: format!("MP3 {rate}, {channels} ch, {sample_rate} Hz"),
            codec,
            channels,
            sample_rate,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(codec: &str, profile: Option<&str>, channels: u32, rate: u32) -> media::Audio {
        media::Audio {
            index: 1,
            codec_name: codec.into(),
            profile: profile.map(String::from),
            channels,
            sample_rate: rate,
            bit_rate: Some(128_000),
        }
    }

    fn with_bit_rate(audio: media::Audio, bit_rate: Option<u64>) -> media::Audio {
        media::Audio { bit_rate, ..audio }
    }

    fn lc() -> media::Audio {
        input("aac", Some("LC"), 2, 48000)
    }

    fn plan(text: &str, input: &media::Audio) -> AudioPlan {
        parse(text).unwrap().plan(Some(input)).unwrap()
    }

    fn copied(plan: &AudioPlan) -> bool {
        matches!(plan.action, AudioAction::Copy { .. })
    }

    fn encoded(plan: &AudioPlan) -> (Codec, u32, u32) {
        match plan.action {
            AudioAction::Encode {
                codec,
                channels,
                sample_rate,
                ..
            } => (codec, channels, sample_rate),
            _ => panic!("not encoded: {}", plan.label),
        }
    }

    fn aac(plan: &AudioPlan) -> u64 {
        match encoded(plan).0 {
            Codec::Aac { bitrate } => bitrate,
            other => panic!("not AAC: {other:?}"),
        }
    }

    fn mp3_codec(plan: &AudioPlan) -> Codec {
        match encoded(plan).0 {
            Codec::Aac { .. } => panic!("not MP3: {}", plan.label),
            codec => codec,
        }
    }

    #[test]
    fn keep_copies_the_same_codec() {
        let p = plan("aac", &lc());
        assert_eq!(p.action, AudioAction::Copy { index: 1 });
        assert_eq!(p.label, "copy (AAC-LC)");
        let p = plan("mp3,bitrate=160k", &input("mp3", None, 1, 44100));
        assert!(copied(&p));
        assert_eq!(p.label, "copy (MP3)");
    }

    #[test]
    fn keep_encodes_inputs_above_the_bitrate() {
        let p = plan("aac,bitrate=96k", &lc());
        assert!(p.label.ends_with("(input is 128 kbit/s)"), "{}", p.label);
        assert_eq!(aac(&p), 96_000);
        let p = plan("aac", &with_bit_rate(lc(), Some(256_000)));
        assert_eq!(aac(&p), 160_000);
        let p = plan("aac,bitrate=128k", &with_bit_rate(lc(), Some(128_070)));
        assert!(copied(&p));
        let p = plan("aac,bitrate=128k", &with_bit_rate(lc(), Some(134_400)));
        assert!(copied(&p));
        let p = plan("aac,bitrate=128k", &with_bit_rate(lc(), Some(134_401)));
        assert_eq!(aac(&p), 128_000);
        let mp3 = input("mp3", None, 2, 22050);
        assert!(copied(&plan("mp3", &with_bit_rate(mp3, Some(160_000)))));
        let mp3 = input("mp3", None, 2, 22050);
        assert_eq!(
            mp3_codec(&plan("mp3", &with_bit_rate(mp3, Some(192_000)))),
            Codec::Mp3Cbr { bitrate: 160_000 }
        );
    }

    #[test]
    fn keep_encodes_when_the_bitrate_is_unknown() {
        let p = plan("aac", &with_bit_rate(lc(), None));
        assert_eq!(aac(&p), 160_000);
        assert!(p.label.ends_with("(input bitrate unknown)"), "{}", p.label);
    }

    #[test]
    fn keep_ignores_bitrate_with_vbr() {
        let mp3 = with_bit_rate(input("mp3", None, 2, 44100), Some(320_000));
        assert!(copied(&plan("mp3,vbr=2", &mp3)));
        let mp3 = with_bit_rate(input("mp3", None, 2, 44100), None);
        assert!(copied(&plan("mp3,vbr=2", &mp3)));
    }

    #[test]
    fn keep_encodes_a_different_codec() {
        let p = plan("aac", &input("mp3", None, 2, 44100));
        assert_eq!(aac(&p), 160_000);
        assert!(p.label.ends_with("(input is MP3)"), "{}", p.label);
        let p = plan("mp3", &lc());
        assert_eq!(mp3_codec(&p), Codec::Mp3Cbr { bitrate: 192_000 });
        assert!(p.label.ends_with("(input is AAC-LC)"), "{}", p.label);
        let p = plan("aac,bitrate=96k", &input("opus", None, 2, 48000));
        assert_eq!(aac(&p), 96_000);
        assert!(p.label.ends_with("(input is opus)"), "{}", p.label);
    }

    #[test]
    fn keep_encodes_what_the_player_cannot_take() {
        let p = plan("aac", &input("aac", Some("HE-AAC"), 2, 48000));
        assert_eq!(aac(&p), 160_000);
        assert!(p.label.ends_with("(input is HE-AAC)"), "{}", p.label);
        let p = plan("aac", &input("aac", Some("LC"), 6, 48000));
        assert!(p.label.ends_with("(input has 6 channels)"), "{}", p.label);
        assert_eq!(encoded(&p).1, 2);
        let p = plan("mp3", &input("mp3", None, 2, 96000));
        assert_eq!(encoded(&p).2, 48000);
        let p = plan("mp3", &input("mp3", None, 1, 8000));
        assert!(p.label.ends_with("(input is 8000 Hz)"), "{}", p.label);
        assert_eq!(encoded(&p).2, 16000);
    }

    #[test]
    fn keep_none_always_encodes() {
        let p = plan("aac,keep=none", &lc());
        assert_eq!(aac(&p), 160_000);
        assert!(!p.label.contains("("), "{}", p.label);
        let p = plan(
            "mp3,keep=none,vbr=2,channels=1",
            &input("mp3", None, 2, 44100),
        );
        assert_eq!(encoded(&p), (Codec::Mp3Vbr { quality: 2 }, 1, 44100));
    }

    #[test]
    fn aac_always_encodes() {
        let p = plan("aac,keep=none,channels=1,samplerate=44.1k", &lc());
        assert_eq!(
            p.action,
            AudioAction::Encode {
                index: 1,
                codec: Codec::Aac { bitrate: 160_000 },
                channels: 1,
                sample_rate: 44100,
            }
        );
    }

    #[test]
    fn mp3_always_encodes() {
        let p = plan("mp3,keep=none", &input("mp3", None, 2, 44100));
        assert_eq!(encoded(&p), (Codec::Mp3Cbr { bitrate: 192_000 }, 2, 44100));
        assert_eq!(p.label, "MP3 192 kbit/s CBR, 2 ch, 44100 Hz");
        let p = plan("mp3,samplerate=22.05k", &lc());
        assert_eq!(mp3_codec(&p), Codec::Mp3Cbr { bitrate: 160_000 });
        let p = plan("mp3,keep=none", &input("mp3", None, 1, 16000));
        assert_eq!(mp3_codec(&p), Codec::Mp3Cbr { bitrate: 160_000 });
        let p = plan("mp3,vbr=0,samplerate=22.05k", &lc());
        assert_eq!(encoded(&p), (Codec::Mp3Vbr { quality: 0 }, 2, 22050));
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
        assert_eq!(p.action, AudioAction::None);
        assert_eq!(p.index(), None);
        let p = parse("aac").unwrap().plan(None).unwrap();
        assert_eq!(p.label, "none (input has no audio)");
    }

    #[test]
    fn aac_rates_can_be_limited() {
        let low = input("aac", Some("LC"), 1, 8000);
        let only = [44100, 48000];
        let p = parse("aac").unwrap().plan_for(Some(&low), &only).unwrap();
        assert_eq!(encoded(&p), (Codec::Aac { bitrate: 160_000 }, 1, 44100));
        let p = parse("aac,keep=none")
            .unwrap()
            .plan_for(Some(&lc()), &only)
            .unwrap();
        assert_eq!(encoded(&p).2, 48000);
        let err = parse("aac,keep=none,samplerate=22.05k")
            .unwrap()
            .plan_for(Some(&lc()), &only)
            .err()
            .unwrap();
        assert!(err.to_string().contains("supported: 44.1k, 48k"), "{err}");
        assert!(parse("aac").unwrap().plan_for(Some(&low), &[]).is_err());
        let p = parse("aac").unwrap().plan_for(Some(&lc()), &[]).unwrap();
        assert_eq!(p.action, AudioAction::Copy { index: 1 });
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
