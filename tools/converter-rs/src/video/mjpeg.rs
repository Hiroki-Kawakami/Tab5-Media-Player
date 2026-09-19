// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::rotation::{self, RotationSpec};
use super::{PictureSpec, VideoOutput, VideoPlan};
use crate::framerate::Rate;
use crate::jpeg::{HuffmanMode, Limits};
use crate::probe;
use crate::size::Constraints;
use crate::spec::{Spec, int_in, one_of, quantity};

pub const KEYS: [&str; 9] = [
    "quality",
    "minquality",
    "bitrate",
    "buffer",
    "maxframe",
    "huffman",
    rotation::KEYS[0],
    rotation::KEYS[1],
    rotation::KEYS[2],
];
pub const PLAYER_MAX_FRAME: usize = 1 << 20;
const PLAYER_READ_AHEAD: usize = 16 * 64 * 1024;
const DEFAULT_QUALITY: u8 = 80;
const DEFAULT_MIN_QUALITY: u8 = 30;
const DEFAULT_BITRATE: u64 = 24_000_000;
const SCALE_OPTIONS: &str = ":out_color_matrix=bt601:out_range=full";

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

pub struct MjpegJob {
    pub filter: String,
    pub width: usize,
    pub height: usize,
    pub rate: Rate,
    pub display_rotation: Option<i32>,
    pub settings: Settings,
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
            },
        })
    }

    pub fn plan(&self, video: &probe::Video) -> Result<VideoPlan> {
        let picture =
            self.picture
                .resolve("mjpeg", video, true, SCALE_OPTIONS, Some(&self.rotation))?;
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
        Ok(VideoPlan {
            index: video.index,
            encoder: None,
            label: format!(
                "MJPEG {}, quality {} (down to {}) within {} Mbit/s over a {} buffer, frames up to {}, {huffman} Huffman",
                picture.label,
                s.quality,
                s.min_quality,
                s.bitrate as f64 / 1e6,
                bytes(s.buffer),
                bytes(s.max_frame),
            ),
            output: VideoOutput::Mjpeg(MjpegJob {
                filter: picture.filter,
                width: picture.width as usize,
                height: picture.height as usize,
                rate: picture.rate,
                display_rotation: picture.rotation.and_then(|r| r.display_rotation),
                settings: self.settings,
            }),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::parse;
    use super::super::tests::source;
    use super::*;

    fn job(text: &str) -> MjpegJob {
        match parse(text).unwrap().plan(&source()).unwrap().output {
            VideoOutput::Mjpeg(job) => job,
            VideoOutput::Ffmpeg(_) => panic!("mjpeg must use the built-in encoder"),
        }
    }

    #[test]
    fn defaults() {
        let j = job("mjpeg");
        assert_eq!((j.width, j.height), (720, 1280));
        assert_eq!(
            j.filter,
            "fps=30000/1001,scale=1280:720:out_color_matrix=bt601:out_range=full,setsar=1,transpose=cclock"
        );
        assert_eq!(j.display_rotation, Some(-90));
        assert_eq!(j.rate, Rate::new(30000, 1001).unwrap());
        assert_eq!(j.settings.quality, 80);
        assert_eq!(j.settings.min_quality, 30);
        assert_eq!(j.settings.max_frame, PLAYER_MAX_FRAME);
        assert_eq!(j.settings.huffman, HuffmanMode::Optimal);
        assert_eq!(j.settings.bitrate, 24_000_000);
        assert_eq!(j.settings.buffer, 1 << 20);
    }

    #[test]
    fn options() {
        let j = job(
            "mjpeg,quality=90,maxframe=80k,minquality=50,huffman=standard,maxfps=24,long=640,rotate=0",
        );
        assert_eq!((j.width, j.height), (640, 360));
        assert!(j.filter.starts_with("fps=24,scale=640:360:"));
        assert_eq!(j.settings.quality, 90);
        assert_eq!(j.settings.min_quality, 50);
        assert_eq!(j.settings.max_frame, 80_000);
        assert_eq!(j.settings.huffman, HuffmanMode::Standard);
        assert_eq!(job("mjpeg,quality=20").settings.min_quality, 20);
        let j = job("mjpeg,bitrate=8M,buffer=512k");
        assert_eq!(j.settings.bitrate, 8_000_000);
        assert_eq!(j.settings.buffer, 512_000);
    }

    #[test]
    fn rotation() {
        let portrait = probe::Video {
            display_width: 1080.0,
            display_height: 1920.0,
            ..source()
        };
        let plan = |text: &str, video: &probe::Video| match parse(text)
            .unwrap()
            .plan(video)
            .unwrap()
            .output
        {
            VideoOutput::Mjpeg(job) => job,
            VideoOutput::Ffmpeg(_) => unreachable!(),
        };
        let j = plan("mjpeg", &portrait);
        assert_eq!((j.width, j.height, j.display_rotation), (720, 1280, None));
        assert!(!j.filter.contains("transpose"));
        let j = plan("mjpeg,rotatewhen=always,rotate=-90", &portrait);
        assert_eq!(
            (j.width, j.height, j.display_rotation),
            (1280, 720, Some(90))
        );
        assert!(j.filter.ends_with(",transpose=clock"));
        let j = plan("mjpeg,rotatemeta=no", &source());
        assert_eq!((j.width, j.height, j.display_rotation), (720, 1280, None));
        let j = plan("mjpeg,rotate=180", &source());
        assert_eq!(
            (j.width, j.height, j.display_rotation),
            (1280, 720, Some(180))
        );
        assert!(j.filter.ends_with(",hflip,vflip"));
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
