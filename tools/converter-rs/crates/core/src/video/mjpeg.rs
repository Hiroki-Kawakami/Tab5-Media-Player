// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::rotation::{self, RotationSpec};
use super::{Color, PictureSpec, VideoCodec, VideoPlan};
use crate::jpeg::{HuffmanMode, Limits};
use crate::media;
use crate::size::Constraints;
use crate::spec::{Spec, int_in, one_of, quantity, yes_no};

pub const KEYS: [&str; 10] = [
    "quality",
    "minquality",
    "bitrate",
    "buffer",
    "maxframe",
    "huffman",
    "dedup",
    rotation::KEYS[0],
    rotation::KEYS[1],
    rotation::KEYS[2],
];
pub const PLAYER_MAX_FRAME: usize = 1 << 20;
const PLAYER_READ_AHEAD: usize = 16 * 64 * 1024;
const DEFAULT_QUALITY: u8 = 75;
const DEFAULT_MIN_QUALITY: u8 = 30;
const DEFAULT_BITRATE: u64 = 24_000_000;

static LIMITS: Constraints = Constraints {
    align: 2,
    max_width: 2560,
    max_height: u32::MAX,
    max_macroblocks: None,
    max_pixels: Some(1920 * 1088),
    default_long: 1280,
    default_short: Some(720),
};

#[derive(Clone, Copy, Debug)]
pub struct Settings {
    pub quality: u8,
    pub min_quality: u8,
    pub bitrate: u64,
    pub buffer: usize,
    pub max_frame: usize,
    pub huffman: HuffmanMode,
    pub dedup: bool,
}

impl Settings {
    pub fn limits(&self) -> Limits {
        Limits {
            min_quality: self.min_quality,
            max_frame: self.max_frame,
            huffman: self.huffman,
        }
    }
}

pub struct Mjpeg {
    picture: PictureSpec,
    rotation: RotationSpec,
    settings: Settings,
}

impl Mjpeg {
    pub fn take(spec: &mut Spec) -> Result<Self> {
        let picture = PictureSpec::take(spec, &LIMITS)?;
        let quality = spec
            .take("quality", |v| int_in(v, 1, 100))?
            .map_or(DEFAULT_QUALITY, |q| q as u8);
        let max_frame = spec
            .take("maxframe", |v| {
                let bytes = quantity(v)?;
                if bytes > PLAYER_MAX_FRAME as u64 {
                    bail!("must be at most {PLAYER_MAX_FRAME} (the player's limit)");
                }
                Ok(bytes as usize)
            })?
            .unwrap_or(PLAYER_MAX_FRAME);
        let min_quality = match spec.take("minquality", |v| int_in(v, 1, 100))? {
            Some(q) if q as u8 > quality => {
                bail!("mjpeg: minquality={q} is above quality={quality}")
            }
            Some(q) => q as u8,
            None => DEFAULT_MIN_QUALITY.min(quality),
        };
        let bitrate = spec.take("bitrate", quantity)?.unwrap_or(DEFAULT_BITRATE);
        let buffer = spec
            .take("buffer", quantity)?
            .map_or(PLAYER_READ_AHEAD, |b| b as usize);
        let huffman = match spec.take("huffman", |v| one_of(v, &["optimal", "standard"]))? {
            Some("standard") => HuffmanMode::Standard,
            _ => HuffmanMode::Optimal,
        };
        let dedup = spec.take("dedup", yes_no)?.unwrap_or(true);
        let rotation = RotationSpec::take(spec)?;
        Ok(Self {
            picture,
            rotation,
            settings: Settings {
                quality,
                min_quality,
                bitrate,
                buffer,
                max_frame,
                huffman,
                dedup,
            },
        })
    }

    pub fn plan(&self, video: &media::Video) -> Result<VideoPlan> {
        let picture =
            self.picture
                .resolve("mjpeg", video, true, Color::Bt601Full, Some(&self.rotation))?;
        let s = &self.settings;
        let bytes = |b: usize| {
            if b.is_multiple_of(1 << 20) {
                format!("{} MiB", b >> 20)
            } else {
                format!("{b} bytes")
            }
        };
        let huffman = match s.huffman {
            HuffmanMode::Optimal => "optimal",
            HuffmanMode::Standard => "standard",
        };
        let repeats = if s.dedup {
            ", unchanged frames merged"
        } else {
            ""
        };
        Ok(VideoPlan {
            index: video.index,
            label: format!(
                "MJPEG {}, quality {} (down to {}) within {} Mbit/s over a {} buffer, frames up to {}, {huffman} Huffman{repeats}",
                picture.label,
                s.quality,
                s.min_quality,
                s.bitrate as f64 / 1e6,
                bytes(s.buffer),
                bytes(s.max_frame),
            ),
            picture,
            codec: VideoCodec::Mjpeg(self.settings),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::parse;
    use super::super::tests::source;
    use super::super::{Picture, rotation::Rotation};
    use super::*;
    use crate::framerate::Rate;

    fn plan_for(text: &str, video: &media::Video) -> (Picture, Settings) {
        let plan = parse(text).unwrap().plan(video).unwrap();
        match plan.codec {
            VideoCodec::Mjpeg(settings) => (plan.picture, settings),
            _ => panic!("mjpeg must use the built-in encoder"),
        }
    }

    fn plan(text: &str) -> (Picture, Settings) {
        plan_for(text, &source())
    }

    fn stored(p: &Picture) -> (u32, u32, Option<i32>) {
        (
            p.width,
            p.height,
            p.rotation.and_then(|r| r.display_rotation),
        )
    }

    #[test]
    fn defaults() {
        let (p, s) = plan("mjpeg");
        assert_eq!((p.width, p.height), (720, 1280));
        assert_eq!((p.resize.width, p.resize.height), (1280, 720));
        assert_eq!(
            p.rotation,
            Some(Rotation {
                degrees: 90,
                display_rotation: Some(-90)
            })
        );
        assert_eq!(p.color, Color::Bt601Full);
        assert_eq!(p.rate, Rate::new(30000, 1001).unwrap());
        assert_eq!(p.convert_rate, Rate::new(30000, 1001));
        assert_eq!(s.quality, 75);
        assert_eq!(s.min_quality, 30);
        assert_eq!(s.max_frame, PLAYER_MAX_FRAME);
        assert_eq!(s.huffman, HuffmanMode::Optimal);
        assert_eq!(s.bitrate, 24_000_000);
        assert_eq!(s.buffer, 1 << 20);
        assert!(s.dedup);
    }

    #[test]
    fn options() {
        let (p, s) = plan(
            "mjpeg,quality=90,maxframe=80k,minquality=50,huffman=standard,maxfps=24,long=640,rotate=0",
        );
        assert_eq!((p.width, p.height), (640, 360));
        assert_eq!(p.convert_rate, Rate::new(24, 1));
        assert_eq!(p.rotation, None);
        assert_eq!(s.quality, 90);
        assert_eq!(s.min_quality, 50);
        assert_eq!(s.max_frame, 80_000);
        assert_eq!(s.huffman, HuffmanMode::Standard);
        assert_eq!(plan("mjpeg,quality=20").1.min_quality, 20);
        let (_, s) = plan("mjpeg,bitrate=8M,buffer=512k");
        assert_eq!(s.bitrate, 8_000_000);
        assert_eq!(s.buffer, 512_000);
        assert!(!plan("mjpeg,dedup=no").1.dedup);
    }

    #[test]
    fn rotation() {
        let portrait = media::Video {
            display_width: 1080.0,
            display_height: 1920.0,
            ..source()
        };
        let (p, _) = plan_for("mjpeg", &portrait);
        assert_eq!(stored(&p), (720, 1280, None));
        assert_eq!(p.rotation, None);
        let (p, _) = plan_for("mjpeg,rotatewhen=always,rotate=-90", &portrait);
        assert_eq!(stored(&p), (1280, 720, Some(90)));
        assert_eq!(p.rotation.unwrap().degrees, 270);
        let (p, _) = plan("mjpeg,rotatemeta=no");
        assert_eq!(stored(&p), (720, 1280, None));
        let (p, _) = plan("mjpeg,rotate=180");
        assert_eq!(stored(&p), (1280, 720, Some(180)));
        assert_eq!(p.rotation.unwrap().degrees, 180);
    }

    #[test]
    fn limits_apply_to_the_stored_size() {
        let text = "mjpeg,width=700,height=2600,scale=stretch,rotatewhen=always";
        let err = parse(text).unwrap().plan(&source()).err().unwrap();
        assert!(format!("{err:#}").contains("output 2600x700"), "{err:#}");
        assert!(
            parse(&format!("{text},rotate=0"))
                .unwrap()
                .plan(&source())
                .is_ok()
        );
    }

    #[test]
    fn errors() {
        assert!(parse("mjpeg,quality=0").is_err());
        assert!(parse("mjpeg,quality=101").is_err());
        assert!(parse("mjpeg,maxframe=2M").is_err());
        assert!(parse("mjpeg,maxframe=1048577").is_err());
        assert!(parse("mjpeg,maxframe=1048576").is_ok());
        assert!(parse("mjpeg,quality=50,minquality=60").is_err());
        assert!(parse("mjpeg,huffman=fast").is_err());
        assert!(parse("mjpeg,dedup=off").is_err());
        assert!(parse("mjpeg,crf=20").is_err());
        assert!(parse("mjpeg,bitrate=0").is_err());
        assert!(parse("mjpeg,buffer=fast").is_err());
        let err = parse("mjpeg,long=2560,short=1440,scale=fit")
            .unwrap()
            .plan(&source())
            .err()
            .unwrap();
        assert!(format!("{err:#}").starts_with("mjpeg: output"), "{err:#}");
    }
}
