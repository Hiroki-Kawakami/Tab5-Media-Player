// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod h264;
mod mpeg2;

use anyhow::{Result, bail};

use crate::probe;
use crate::size::{self, Constraints};
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
const FALLBACK_FPS: f64 = 30.0;

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
    let keys: Vec<&str> = size::KEYS.iter().chain(codec_keys).copied().collect();
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

fn keyint_frames(video: &probe::Video, seconds: f64) -> u32 {
    let fps = video.fps.unwrap_or(FALLBACK_FPS);
    ((fps * seconds).round() as u32).max(1)
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
    fn unknown_codec_and_keys() {
        assert!(parse("h265").is_err());
        let err = parse("mpeg2,crf=20").err().unwrap().to_string();
        assert!(
            err.contains("unknown key 'crf'") && err.contains("width"),
            "{err}"
        );
    }
}
