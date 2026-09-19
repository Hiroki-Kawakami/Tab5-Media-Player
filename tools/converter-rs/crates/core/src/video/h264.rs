// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::{Color, DECODER_LIMITS, PictureSpec, VideoCodec, VideoPlan};
use crate::media;
use crate::spec::{Spec, int_in, one_of, quantity, seconds};

pub const KEYS: [&str; 5] = ["profile", "crf", "bitrate", "keyint", "preset"];
const DEFAULT_CRF: u32 = 32;
const DEFAULT_KEYINT: f64 = 4.0;
const PRESETS: [&str; 10] = [
    "ultrafast",
    "superfast",
    "veryfast",
    "faster",
    "fast",
    "medium",
    "slow",
    "slower",
    "veryslow",
    "placebo",
];

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Profile {
    Baseline,
    Main,
    High,
}

impl Profile {
    fn parse(value: &str) -> Result<Self> {
        Ok(match one_of(value, &["baseline", "main", "high"])? {
            "baseline" => Self::Baseline,
            "high" => Self::High,
            _ => Self::Main,
        })
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Baseline => "baseline",
            Self::Main => "main",
            Self::High => "high",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Rate {
    Crf(u32),
    Bitrate(u64),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Params {
    pub profile: Profile,
    pub rate: Rate,
    pub keyint: u32,
    pub preset: &'static str,
}

pub struct H264 {
    picture: PictureSpec,
    profile: Profile,
    rate: Rate,
    keyint: f64,
    preset: &'static str,
}

impl H264 {
    pub fn take(spec: &mut Spec) -> Result<Self> {
        let picture = PictureSpec::take(spec, &DECODER_LIMITS)?;
        let profile = spec
            .take("profile", Profile::parse)?
            .unwrap_or(Profile::Main);
        let crf = spec.take("crf", |v| int_in(v, 0, 51))?;
        let bitrate = spec.take("bitrate", quantity)?;
        let rate = match (crf, bitrate) {
            (Some(_), Some(_)) => bail!("h264: crf and bitrate cannot be combined"),
            (_, Some(bitrate)) => Rate::Bitrate(bitrate),
            (crf, None) => Rate::Crf(crf.unwrap_or(DEFAULT_CRF)),
        };
        let keyint = spec.take("keyint", seconds)?.unwrap_or(DEFAULT_KEYINT);
        let preset = spec
            .take("preset", |v| one_of(v, &PRESETS))?
            .unwrap_or("medium");
        Ok(Self {
            picture,
            profile,
            rate,
            keyint,
            preset,
        })
    }

    pub fn plan(&self, video: &media::Video) -> Result<VideoPlan> {
        let picture = self
            .picture
            .resolve("h264", video, false, Color::Source, None)?;
        let keyint = picture.keyint(self.keyint);
        let rate = match self.rate {
            Rate::Crf(crf) => format!("crf {crf}"),
            Rate::Bitrate(bitrate) => format!("{} kbit/s", bitrate as f64 / 1000.0),
        };
        Ok(VideoPlan {
            index: video.index,
            label: format!(
                "H.264 {} {}, {rate}, keyframe every {keyint} frames",
                self.profile.name(),
                picture.label
            ),
            picture,
            codec: VideoCodec::H264(Params {
                profile: self.profile,
                rate: self.rate,
                keyint,
                preset: self.preset,
            }),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::parse;
    use super::super::tests::{plan, source};
    use super::*;

    fn params(text: &str) -> Params {
        match plan(text).codec {
            VideoCodec::H264(p) => p,
            _ => unreachable!(),
        }
    }

    #[test]
    fn defaults() {
        let p = plan("h264");
        assert_eq!((p.picture.width, p.picture.height), (640, 360));
        assert_eq!(p.picture.convert_rate, None);
        assert_eq!(p.picture.color, Color::Source);
        assert_eq!(
            params("h264"),
            Params {
                profile: Profile::Main,
                rate: Rate::Crf(32),
                keyint: 120,
                preset: "medium",
            }
        );
    }

    #[test]
    fn options() {
        assert_eq!(params("h264,profile=baseline").profile, Profile::Baseline);
        assert_eq!(
            params("h264,profile=high,bitrate=1.5M,keyint=0.5,preset=fast"),
            Params {
                profile: Profile::High,
                rate: Rate::Bitrate(1_500_000),
                keyint: 15,
                preset: "fast",
            }
        );
    }

    #[test]
    fn errors() {
        assert!(parse("h264,crf=20,bitrate=1M").is_err());
        assert!(parse("h264,profile=extended").is_err());
        assert!(parse("h264,preset=quick").is_err());
        assert!(parse("h264,quality=85").is_err());
        let err = parse("h264,long=1920")
            .unwrap()
            .plan(&source())
            .err()
            .unwrap();
        assert!(
            format!("{err:#}").starts_with("h264: output 1920x1080"),
            "{err:#}"
        );
    }
}
