// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::{PictureSpec, VideoOutput, VideoPlan};
use crate::framerate::Rate;
use crate::jpeg::{HuffmanMode, Settings};
use crate::probe;
use crate::size::Constraints;
use crate::spec::{Spec, int_in, one_of, quantity};

pub const KEYS: [&str; 4] = ["quality", "maxframe", "minquality", "huffman"];
pub const PLAYER_MAX_FRAME: usize = 1 << 20;
const DEFAULT_QUALITY: u8 = 80;
const DEFAULT_MIN_QUALITY: u8 = 30;
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

pub struct MjpegJob {
    pub filter: String,
    pub width: usize,
    pub height: usize,
    pub rate: Rate,
    pub settings: Settings,
}

pub struct Mjpeg {
    picture: PictureSpec,
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
        let huffman = match spec.take("huffman", |v| one_of(v, &["optimal", "standard"]))? {
            Some("standard") => HuffmanMode::Standard,
            _ => HuffmanMode::Optimal,
        };
        Ok(Self {
            picture,
            settings: Settings {
                quality,
                min_quality,
                max_frame,
                huffman,
            },
        })
    }

    pub fn plan(&self, video: &probe::Video) -> Result<VideoPlan> {
        let picture = self.picture.resolve("mjpeg", video, true, SCALE_OPTIONS)?;
        let s = &self.settings;
        let limit = if s.max_frame == PLAYER_MAX_FRAME {
            "1 MiB".to_string()
        } else {
            format!("{} bytes", s.max_frame)
        };
        let huffman = match s.huffman {
            HuffmanMode::Optimal => "optimal",
            HuffmanMode::Standard => "standard",
        };
        Ok(VideoPlan {
            index: video.index,
            encoder: None,
            label: format!(
                "MJPEG {}, quality {} (down to {} to stay within {limit} per frame), {huffman} Huffman",
                picture.label, s.quality, s.min_quality
            ),
            output: VideoOutput::Mjpeg(MjpegJob {
                filter: picture.filter,
                width: picture.width as usize,
                height: picture.height as usize,
                rate: picture.rate,
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
        assert_eq!((j.width, j.height), (1280, 720));
        assert_eq!(
            j.filter,
            "fps=30000/1001,scale=1280:720:out_color_matrix=bt601:out_range=full,setsar=1"
        );
        assert_eq!(j.rate, Rate::new(30000, 1001).unwrap());
        assert_eq!(j.settings.quality, 80);
        assert_eq!(j.settings.min_quality, 30);
        assert_eq!(j.settings.max_frame, PLAYER_MAX_FRAME);
        assert_eq!(j.settings.huffman, HuffmanMode::Optimal);
    }

    #[test]
    fn options() {
        let j =
            job("mjpeg,quality=90,maxframe=80k,minquality=50,huffman=standard,maxfps=24,long=640");
        assert_eq!((j.width, j.height), (640, 360));
        assert!(j.filter.starts_with("fps=24,scale=640:360:"));
        assert_eq!(j.settings.quality, 90);
        assert_eq!(j.settings.min_quality, 50);
        assert_eq!(j.settings.max_frame, 80_000);
        assert_eq!(j.settings.huffman, HuffmanMode::Standard);
        assert_eq!(job("mjpeg,quality=20").settings.min_quality, 20);
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
        let err = parse("mjpeg,long=2560,short=1440,scale=fit")
            .unwrap()
            .plan(&source())
            .err()
            .unwrap();
        assert!(format!("{err:#}").starts_with("mjpeg: output"), "{err:#}");
    }
}
