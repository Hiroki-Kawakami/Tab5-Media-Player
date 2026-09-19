// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::{Color, DECODER_LIMITS, PictureSpec, VideoCodec, VideoPlan};
use crate::media;
use crate::spec::{Spec, int_in, one_of, quantity, seconds, yes_no};

pub const KEYS: [&str; 6] = ["qscale", "bitrate", "bframes", "keyint", "gop", "hq"];
const MAX_BFRAMES: u32 = 3;
const DEFAULT_QSCALE: u32 = 8;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Rate {
    Qscale(u32),
    Bitrate(u64),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Params {
    pub rate: Rate,
    pub bframes: u32,
    pub keyint: u32,
    pub closed_gop: bool,
    pub hq: bool,
}

pub struct Mpeg2 {
    picture: PictureSpec,
    rate: Rate,
    bframes: u32,
    keyint: f64,
    closed_gop: bool,
    hq: bool,
}

impl Mpeg2 {
    pub fn take(spec: &mut Spec) -> Result<Self> {
        let picture = PictureSpec::take(spec, &DECODER_LIMITS)?;
        let qscale = spec.take("qscale", |v| int_in(v, 1, 31))?;
        let bitrate = spec.take("bitrate", quantity)?;
        let rate = match (qscale, bitrate) {
            (Some(_), Some(_)) => bail!("mpeg2: qscale and bitrate cannot be combined"),
            (_, Some(bitrate)) => Rate::Bitrate(bitrate),
            (qscale, None) => Rate::Qscale(qscale.unwrap_or(DEFAULT_QSCALE)),
        };
        let bframes = spec
            .take("bframes", |v| int_in(v, 0, MAX_BFRAMES))?
            .unwrap_or(2);
        let keyint = spec.take("keyint", seconds)?.unwrap_or(2.0);
        let closed_gop = spec
            .take("gop", |v| one_of(v, &["closed", "open"]))?
            .is_none_or(|gop| gop == "closed");
        let hq = spec.take("hq", yes_no)?.unwrap_or(true);
        Ok(Self {
            picture,
            rate,
            bframes,
            keyint,
            closed_gop,
            hq,
        })
    }

    pub fn plan(&self, video: &media::Video) -> Result<VideoPlan> {
        let picture = self
            .picture
            .resolve("mpeg2", video, false, Color::Source, None)?;
        let keyint = picture.keyint(self.keyint);
        let rate = match self.rate {
            Rate::Qscale(q) => format!("qscale {q}"),
            Rate::Bitrate(bitrate) => format!("{} kbit/s", bitrate as f64 / 1000.0),
        };
        Ok(VideoPlan {
            index: video.index,
            label: format!(
                "MPEG-2 {}, {rate}, {} B pictures, {} GOP of {keyint} frames{}",
                picture.label,
                self.bframes,
                if self.closed_gop { "closed" } else { "open" },
                if self.hq { ", hq" } else { "" },
            ),
            picture,
            codec: VideoCodec::Mpeg2(Params {
                rate: self.rate,
                bframes: self.bframes,
                keyint,
                closed_gop: self.closed_gop,
                hq: self.hq,
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
            VideoCodec::Mpeg2(p) => p,
            _ => unreachable!(),
        }
    }

    #[test]
    fn defaults() {
        let p = plan("mpeg2");
        assert_eq!((p.picture.width, p.picture.height), (640, 360));
        assert_eq!(p.picture.color, Color::Source);
        assert_eq!(
            params("mpeg2"),
            Params {
                rate: Rate::Qscale(8),
                bframes: 2,
                keyint: 60,
                closed_gop: true,
                hq: true,
            }
        );
    }

    #[test]
    fn options() {
        let text = "mpeg2,bitrate=2M,bframes=0,keyint=1,gop=open,hq=no,short=720";
        let p = plan(text);
        assert_eq!((p.picture.width, p.picture.height), (1280, 720));
        assert_eq!(
            params(text),
            Params {
                rate: Rate::Bitrate(2_000_000),
                bframes: 0,
                keyint: 30,
                closed_gop: false,
                hq: false,
            }
        );
    }

    #[test]
    fn errors() {
        assert!(parse("mpeg2,qscale=4,bitrate=1M").is_err());
        assert!(parse("mpeg2,qscale=0").is_err());
        assert!(parse("mpeg2,qscale=32").is_err());
        assert!(parse("mpeg2,bframes=4").is_err());
        assert!(parse("mpeg2,gop=half").is_err());
        assert!(parse("mpeg2,hq=maybe").is_err());
        let err = parse("mpeg2,width=1280,height=1280,scale=stretch")
            .unwrap()
            .plan(&source())
            .err()
            .unwrap();
        assert!(format!("{err:#}").starts_with("mpeg2: output"), "{err:#}");
    }
}
