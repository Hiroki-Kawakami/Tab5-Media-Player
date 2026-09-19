// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod h264;
mod mjpeg;
mod mpeg2;

use anyhow::{Context, Result, bail};

use crate::framerate::{self, FrameRateSpec, Rate};
use crate::probe;
use crate::size::{self, Constraints, SizeSpec};
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

h264   H.264 (libx264), 4:2:0 8-bit (default)
  profile=P           baseline, main or high (default high)
  crf=N               0-51 (default 23)
  bitrate=R           e.g. 800k or 1.5M; replaces crf
  keyint=S            keyframe interval in seconds (default 2)
  preset=P            x264 preset (default medium)

mpeg2  MPEG-2 video (mpeg2video), Main profile, progressive 4:2:0
  qscale=N            1-31, lower is better (default 4)
  bitrate=R           e.g. 2M; replaces qscale
  bframes=N           consecutive B pictures, 0-3 (default 2)
  keyint=S            I picture interval in seconds (default 2)
  gop=G               closed or open (default closed); closed turns off
                      scene-change I pictures
  hq=B                yes or no (default yes): -mbd rd -trellis 1 -intra_vlc 1

mjpeg  Motion JPEG (built-in encoder), baseline 4:2:0, always constant frame rate
  quality=N           1-100 (default 80)
  minquality=N        lowest quality rate control may use (default 30)
  bitrate=R           video bitrate to stay under, e.g. 12M (default 24M);
                      quality drops where the estimate would exceed it
  buffer=B            how far a burst may run ahead of bitrate, in bytes
                      (default 1048576, the player's read-ahead)
  maxframe=B          largest frame in bytes, e.g. 80k (default and maximum
                      1048576, the player's limit); a frame over it is
                      re-quantised at a lower quality
  huffman=H           optimal (per-frame tables, default) or standard
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

pub use mjpeg::{MjpegJob, PLAYER_MAX_FRAME, Settings as MjpegSettings};

pub enum VideoOutput {
    Ffmpeg(Vec<String>),
    Mjpeg(MjpegJob),
}

pub struct VideoPlan {
    pub index: u32,
    pub encoder: Option<&'static str>,
    pub label: String,
    pub output: VideoOutput,
}

pub enum VideoSpec {
    H264(h264::H264),
    Mpeg2(mpeg2::Mpeg2),
    Mjpeg(mjpeg::Mjpeg),
}

pub fn parse(text: &str) -> Result<VideoSpec> {
    let mut spec = Spec::parse(text)?;
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
    pub fn plan(&self, video: &probe::Video) -> Result<VideoPlan> {
        match self {
            Self::H264(h264) => h264.plan(video),
            Self::Mpeg2(mpeg2) => mpeg2.plan(video),
            Self::Mjpeg(mjpeg) => mjpeg.plan(video),
        }
    }
}

struct PictureSpec {
    size: SizeSpec,
    framerate: FrameRateSpec,
    constraints: &'static Constraints,
}

struct Picture {
    filter: String,
    label: String,
    width: u32,
    height: u32,
    rate: Rate,
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
        video: &probe::Video,
        constant_rate: bool,
        scale_options: &str,
    ) -> Result<Picture> {
        let resize = self
            .size
            .resolve(video.display_width, video.display_height, self.constraints)
            .context(codec)?;
        let framerate = self.framerate.resolve(video.fps);
        let rate_filter = if constant_rate {
            Some(framerate.cfr_filter())
        } else {
            framerate.filter()
        };
        let filter = rate_filter
            .into_iter()
            .chain([resize.filter(scale_options)])
            .collect::<Vec<_>>()
            .join(",");
        Ok(Picture {
            filter,
            label: format!("{}x{} {}", resize.width, resize.height, framerate.label),
            width: resize.width,
            height: resize.height,
            rate: framerate.rate,
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

    pub fn source() -> probe::Video {
        probe::Video {
            index: 0,
            display_width: 1920.0,
            display_height: 1080.0,
            fps: Rate::new(30000, 1001),
        }
    }

    pub fn ffmpeg_args(plan: VideoPlan) -> Vec<String> {
        match plan.output {
            VideoOutput::Ffmpeg(args) => args,
            VideoOutput::Mjpeg(_) => panic!("not an ffmpeg encode"),
        }
    }

    pub fn args(text: &str) -> Vec<String> {
        ffmpeg_args(parse(text).unwrap().plan(&source()).unwrap())
    }

    pub fn has(args: &[String], pair: [&str; 2]) -> bool {
        args.windows(2).any(|w| w[0] == pair[0] && w[1] == pair[1])
    }

    #[test]
    fn frame_rate_is_capped_and_drives_keyint() {
        let fast = probe::Video {
            fps: Rate::new(60, 1),
            ..source()
        };
        for codec in ["h264", "mpeg2"] {
            let a = ffmpeg_args(parse(codec).unwrap().plan(&fast).unwrap());
            assert!(has(&a, ["-vf", "fps=30,scale=640:360,setsar=1"]), "{a:?}");
            assert!(has(&a, ["-g", "60"]), "{a:?}");
            let a = ffmpeg_args(
                parse(&format!("{codec},fps=24,keyint=1"))
                    .unwrap()
                    .plan(&fast)
                    .unwrap(),
            );
            assert!(has(&a, ["-vf", "fps=24,scale=640:360,setsar=1"]), "{a:?}");
            assert!(has(&a, ["-g", "24"]), "{a:?}");
            let a = ffmpeg_args(
                parse(&format!("{codec},maxfps=60"))
                    .unwrap()
                    .plan(&fast)
                    .unwrap(),
            );
            assert!(has(&a, ["-vf", "scale=640:360,setsar=1"]), "{a:?}");
            assert!(has(&a, ["-g", "120"]), "{a:?}");
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
