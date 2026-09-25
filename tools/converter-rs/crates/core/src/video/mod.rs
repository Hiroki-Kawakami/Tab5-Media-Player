// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

pub mod h264;
pub mod mjpeg;
pub mod mpeg2;
pub mod rotation;

use anyhow::{Context, Result, bail};

use crate::framerate::{self, FrameRateSpec, Rate};
use crate::media;
use crate::size::{self, Constraints, Resize, SizeSpec};
use crate::spec::Spec;

pub const HELP: &str = "\
--video <codec>[,key=value]...

size keys, for every codec:
  width=N, height=N   output width / height in pixels
  long=N, short=N     output long / short side, following the input orientation
  scale=MODE          contain  fit inside the box, never upscale (default)
                      fit      fit inside the box, upscale allowed
                      cover    fill the box and crop the overflow (needs both sides)
                      stretch  fill the box ignoring the aspect ratio (needs both sides)
                      Without any size key, long=640 is used (mjpeg: long=1280,short=720);
                      one side keeps the aspect ratio.
  fps=R               output frame rate, e.g. 24, 12.5, 29.97 or 30000/1001;
                      frames are dropped or repeated to match
  maxfps=R            lower the frame rate to R only when the input is faster
                      Without fps or maxfps, maxfps=30 is used.

h264   H.264 (libx264), 4:2:0 8-bit; B pictures are never references and one
       reference frame is used, so the player can drop B pictures when late
  profile=P           baseline, main (default) or high
  crf=N               0-51 (default 32)
  bitrate=R           e.g. 800k or 1.5M; replaces crf
  keyint=S            keyframe interval in seconds (default 4)
  preset=P            x264 preset (default medium)

mpeg2  MPEG-2 video, Main profile, progressive 4:2:0 (mpeg2video; the browser
       version has its own encoder)
  qscale=N            1-31, lower is better (default 8)
  bitrate=R           e.g. 2M; replaces qscale (not in the browser)
  bframes=N           consecutive B pictures, 0-3 (default 2)
  keyint=S            I picture interval in seconds (default 2, at most 600
                      frames)
  gop=G               closed or open (default closed); closed turns off
                      scene-change I pictures (the browser needs closed)
  hq=B                yes or no (default yes): rate-distortion decisions, a
                      slower encode (-mbd rd -trellis 1 -intra_vlc 1)

mjpeg  Motion JPEG (built-in encoder), baseline 4:2:0 (default)
  quality=N           1-100 (default 75)
  minquality=N        lowest quality rate control may use (default 30)
  bitrate=R           video bitrate to stay under, e.g. 12M (default 24M);
                      quality drops where the estimate would exceed it
  buffer=B            how far a burst may run ahead of bitrate, in bytes
                      (default 1048576, the player's read-ahead)
  maxframe=B          largest frame in bytes, e.g. 80k (default and maximum
                      1048576, the player's limit); a frame over it is
                      re-quantised at a lower quality
  huffman=H           optimal (per-frame tables, default) or standard
  dedup=B             yes (default) or no: a frame that differs from the last
                      stored one by less than the quantisation at quality is
                      not stored, and the stored one is shown for longer (at
                      least one frame every 2 seconds is stored)
  rotate=D            turn the picture D degrees counter-clockwise before
                      storing it: 0, 90 (default), 180, 270, -90, -180, -270
  rotatewhen=W        landscape (default), portrait or always: which outputs
                      are turned (sizes are the displayed ones)
  rotatemeta=B        yes (default) or no: store the opposite rotation as
                      display metadata, so players show the original
                      orientation; with rotate=90, a UI at 90 decodes the
                      720x1280 frames straight into the panel
";

const DECODER_LIMITS: Constraints = Constraints {
    align: 2,
    max_width: 1280,
    max_height: 1280,
    max_macroblocks: Some(3600),
    max_pixels: None,
    default_long: 640,
    default_short: None,
};

pub struct VideoPlan {
    pub index: u32,
    pub label: String,
    pub picture: Picture,
    pub codec: VideoCodec,
}

pub enum VideoCodec {
    H264(h264::Params),
    Mpeg2(mpeg2::Params),
    Mjpeg(mjpeg::Settings),
}

pub enum VideoSpec {
    H264(h264::H264),
    Mpeg2(mpeg2::Mpeg2),
    Mjpeg(mjpeg::Mjpeg),
}

#[cfg(test)]
pub fn parse(text: &str) -> Result<VideoSpec> {
    from_spec(Spec::parse(text)?)
}

pub fn from_spec(mut spec: Spec) -> Result<VideoSpec> {
    let (video, codec_keys): (_, &[&str]) = match spec.codec.clone().as_str() {
        "h264" => (VideoSpec::H264(h264::H264::take(&mut spec)?), &h264::KEYS),
        "mpeg2" => (
            VideoSpec::Mpeg2(mpeg2::Mpeg2::take(&mut spec)?),
            &mpeg2::KEYS,
        ),
        "mjpeg" => (
            VideoSpec::Mjpeg(mjpeg::Mjpeg::take(&mut spec)?),
            &mjpeg::KEYS,
        ),
        other => bail!("unknown video codec '{other}' (codecs: h264, mpeg2, mjpeg)"),
    };
    let keys: Vec<&str> = size::KEYS
        .iter()
        .chain(&framerate::KEYS)
        .chain(codec_keys)
        .copied()
        .collect();
    spec.finish(&keys)?;
    Ok(video)
}

impl VideoSpec {
    pub fn plan(&self, video: &media::Video) -> Result<VideoPlan> {
        match self {
            Self::H264(h264) => h264.plan(video),
            Self::Mpeg2(mpeg2) => mpeg2.plan(video),
            Self::Mjpeg(mjpeg) => mjpeg.plan(video),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Color {
    Source,
    Bt601Full,
}

struct PictureSpec {
    size: SizeSpec,
    framerate: FrameRateSpec,
    constraints: &'static Constraints,
}

pub struct Picture {
    pub resize: Resize,
    pub rotation: Option<rotation::Rotation>,
    pub width: u32,
    pub height: u32,
    pub rate: Rate,
    pub convert_rate: Option<Rate>,
    pub color: Color,
    label: String,
}

impl PictureSpec {
    fn take(spec: &mut Spec, constraints: &'static Constraints) -> Result<Self> {
        Ok(Self {
            size: SizeSpec::take(spec, constraints)?,
            framerate: FrameRateSpec::take(spec)?,
            constraints,
        })
    }

    fn resolve(
        &self,
        codec: &'static str,
        video: &media::Video,
        constant_rate: bool,
        color: Color,
        rotation: Option<&rotation::RotationSpec>,
    ) -> Result<Picture> {
        let resize = self
            .size
            .fit(video.display_width, video.display_height, self.constraints);
        let rotation = rotation.and_then(|r| r.resolve(resize.width, resize.height));
        let (width, height) = rotation.map_or((resize.width, resize.height), |r| {
            r.stored(resize.width, resize.height)
        });
        self.constraints.check(width, height).context(codec)?;
        let framerate = self.framerate.resolve(video.fps);
        let convert_rate = if constant_rate {
            Some(framerate.rate)
        } else {
            framerate.convert
        };
        let stored = rotation.map_or(String::new(), |r| {
            format!(" (stored {width}x{height}, {})", r.label())
        });
        Ok(Picture {
            label: format!(
                "{}x{}{stored} {}",
                resize.width, resize.height, framerate.label
            ),
            resize,
            rotation,
            width,
            height,
            rate: framerate.rate,
            convert_rate,
            color,
        })
    }
}

impl Picture {
    fn keyint(&self, seconds: f64) -> u32 {
        ((self.rate.as_f64() * seconds).round() as u32).max(1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    pub fn source() -> media::Video {
        media::Video {
            index: 0,
            display_width: 1920.0,
            display_height: 1080.0,
            fps: Rate::new(30000, 1001),
        }
    }

    pub fn plan(text: &str) -> VideoPlan {
        parse(text).unwrap().plan(&source()).unwrap()
    }

    fn keyint(plan: &VideoPlan) -> u32 {
        match &plan.codec {
            VideoCodec::H264(p) => p.keyint,
            VideoCodec::Mpeg2(p) => p.keyint,
            VideoCodec::Mjpeg(_) => unreachable!(),
        }
    }

    #[test]
    fn frame_rate_is_capped_and_drives_keyint() {
        let fast = media::Video {
            fps: Rate::new(60, 1),
            ..source()
        };
        let plan = |text: &str| parse(text).unwrap().plan(&fast).unwrap();
        for codec in ["h264", "mpeg2"] {
            let p = plan(&format!("{codec},keyint=2"));
            assert_eq!(p.picture.convert_rate, Rate::new(30, 1));
            assert_eq!(p.picture.rate, Rate::new(30, 1).unwrap());
            assert_eq!(keyint(&p), 60);
            let p = plan(&format!("{codec},fps=24,keyint=1"));
            assert_eq!(p.picture.convert_rate, Rate::new(24, 1));
            assert_eq!(keyint(&p), 24);
            let p = plan(&format!("{codec},maxfps=60,keyint=2"));
            assert_eq!(p.picture.convert_rate, None);
            assert_eq!((p.picture.width, p.picture.height), (640, 360));
            assert_eq!(keyint(&p), 120);
        }
        assert!(parse("h264,fps=30,maxfps=30").is_err());
    }

    #[test]
    fn unknown_codec_and_keys() {
        assert!(parse("h265").is_err());
        let err = parse("mpeg2,crf=20").err().unwrap().to_string();
        assert!(
            err.contains("unknown key 'crf'") && err.contains("width"),
            "{err}"
        );
    }
}
