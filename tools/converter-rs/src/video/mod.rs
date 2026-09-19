// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod h264;
mod mpeg2;

use anyhow::{Context, Result, bail};

use crate::framerate::{self, FrameRateSpec};
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
                      Without any size key, long=640 is used; one side keeps the aspect ratio.
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
";

const PLAYER_LIMITS: Constraints = Constraints {
    align: 2,
    max_side: 1280,
    max_macroblocks: 3600,
};

pub struct VideoPlan {
    pub index: u32,
    pub encoder: &'static str,
    pub label: String,
    pub args: Vec<String>,
}

pub enum VideoSpec {
    H264(h264::H264),
    Mpeg2(mpeg2::Mpeg2),
}

pub fn parse(text: &str) -> Result<VideoSpec> {
    let mut spec = Spec::parse(text)?;
    let (video, codec_keys): (_, &[&str]) = match spec.codec.clone().as_str() {
        "h264" => (VideoSpec::H264(h264::H264::take(&mut spec)?), &h264::KEYS),
        "mpeg2" => (
            VideoSpec::Mpeg2(mpeg2::Mpeg2::take(&mut spec)?),
            &mpeg2::KEYS,
        ),
        other => bail!("unknown video codec '{other}' (codecs: h264, mpeg2)"),
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
        }
    }
}

struct PictureSpec {
    size: SizeSpec,
    framerate: FrameRateSpec,
}

struct Picture {
    filter: String,
    label: String,
    fps: f64,
}

impl PictureSpec {
    fn take(spec: &mut Spec) -> Result<Self> {
        Ok(Self {
            size: SizeSpec::take(spec)?,
            framerate: FrameRateSpec::take(spec)?,
        })
    }

    fn resolve(&self, codec: &'static str, video: &probe::Video) -> Result<Picture> {
        let resize = self
            .size
            .resolve(video.display_width, video.display_height, &PLAYER_LIMITS)
            .context(codec)?;
        let framerate = self.framerate.resolve(video.fps);
        let filter = framerate
            .filter()
            .into_iter()
            .chain([resize.filter()])
            .collect::<Vec<_>>()
            .join(",");
        Ok(Picture {
            filter,
            label: format!("{}x{} {}", resize.width, resize.height, framerate.label),
            fps: framerate.fps,
        })
    }
}

impl Picture {
    fn keyint(&self, seconds: f64) -> u32 {
        ((self.fps * seconds).round() as u32).max(1)
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
            fps: Some(30000.0 / 1001.0),
        }
    }

    pub fn args(text: &str) -> Vec<String> {
        parse(text).unwrap().plan(&source()).unwrap().args
    }

    pub fn has(args: &[String], pair: [&str; 2]) -> bool {
        args.windows(2).any(|w| w[0] == pair[0] && w[1] == pair[1])
    }

    #[test]
    fn frame_rate_is_capped_and_drives_keyint() {
        let fast = probe::Video {
            fps: Some(60.0),
            ..source()
        };
        for codec in ["h264", "mpeg2"] {
            let a = parse(codec).unwrap().plan(&fast).unwrap().args;
            assert!(has(&a, ["-vf", "fps=30,scale=640:360,setsar=1"]), "{a:?}");
            assert!(has(&a, ["-g", "60"]), "{a:?}");
            let a = parse(&format!("{codec},fps=24,keyint=1"))
                .unwrap()
                .plan(&fast)
                .unwrap()
                .args;
            assert!(has(&a, ["-vf", "fps=24,scale=640:360,setsar=1"]), "{a:?}");
            assert!(has(&a, ["-g", "24"]), "{a:?}");
            let a = parse(&format!("{codec},maxfps=60"))
                .unwrap()
                .plan(&fast)
                .unwrap()
                .args;
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
