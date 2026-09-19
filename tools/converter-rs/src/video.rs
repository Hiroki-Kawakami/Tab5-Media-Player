// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result, bail};

use crate::probe;
use crate::size::{self, Constraints, SizeSpec};
use crate::spec::{Spec, int_in, one_of, quantity, seconds};

pub const HELP: &str = "\
--video <codec>[,key=value]...

h264   H.264 (libx264), 4:2:0 8-bit
  width=N, height=N   output width / height in pixels
  long=N, short=N     output long / short side, following the input orientation
  scale=MODE          contain  fit inside the box, never upscale (default)
                      fit      fit inside the box, upscale allowed
                      cover    fill the box and crop the overflow (needs both sides)
                      stretch  fill the box ignoring the aspect ratio (needs both sides)
                      Without any size key, long=640 is used; one side keeps the aspect ratio.
  profile=P           baseline, main or high (default high)
  crf=N               0-51 (default 23)
  bitrate=R           e.g. 800k or 1.5M; replaces crf
  keyint=S            keyframe interval in seconds (default 2)
  preset=P            x264 preset (default medium)
";

const FALLBACK_FPS: f64 = 30.0;

pub struct VideoPlan {
    pub index: u32,
    pub encoder: &'static str,
    pub label: String,
    pub args: Vec<String>,
}

pub enum VideoSpec {
    H264(H264),
}

pub fn parse(text: &str) -> Result<VideoSpec> {
    let mut spec = Spec::parse(text)?;
    let video = match spec.codec.clone().as_str() {
        "h264" => VideoSpec::H264(H264::take(&mut spec)?),
        other => bail!("unknown video codec '{other}' (codecs: h264)"),
    };
    spec.finish(&H264::KEYS)?;
    Ok(video)
}

impl VideoSpec {
    pub fn plan(&self, video: &probe::Video) -> Result<VideoPlan> {
        match self {
            Self::H264(h264) => h264.plan(video),
        }
    }
}

enum Rate {
    Crf(u32),
    Bitrate(u64),
}

pub struct H264 {
    size: SizeSpec,
    profile: &'static str,
    rate: Rate,
    keyint: f64,
    preset: &'static str,
}

impl H264 {
    const KEYS: [&str; 10] = [
        size::KEYS[0],
        size::KEYS[1],
        size::KEYS[2],
        size::KEYS[3],
        size::KEYS[4],
        "profile",
        "crf",
        "bitrate",
        "keyint",
        "preset",
    ];
    const CONSTRAINTS: Constraints = Constraints {
        align: 2,
        max_side: 1280,
        max_macroblocks: 3600,
    };
    const PROFILES: [&str; 3] = ["baseline", "main", "high"];
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

    fn take(spec: &mut Spec) -> Result<Self> {
        let size = SizeSpec::take(spec)?;
        let profile = spec
            .take("profile", |v| one_of(v, &Self::PROFILES))?
            .unwrap_or("high");
        let crf = spec.take("crf", |v| int_in(v, 0, 51))?;
        let bitrate = spec.take("bitrate", quantity)?;
        let rate = match (crf, bitrate) {
            (Some(_), Some(_)) => bail!("h264: crf and bitrate cannot be combined"),
            (_, Some(bitrate)) => Rate::Bitrate(bitrate),
            (crf, None) => Rate::Crf(crf.unwrap_or(23)),
        };
        let keyint = spec.take("keyint", seconds)?.unwrap_or(2.0);
        let preset = spec
            .take("preset", |v| one_of(v, &Self::PRESETS))?
            .unwrap_or("medium");
        Ok(Self {
            size,
            profile,
            rate,
            keyint,
            preset,
        })
    }

    fn plan(&self, video: &probe::Video) -> Result<VideoPlan> {
        let resize = self
            .size
            .resolve(
                video.display_width,
                video.display_height,
                &Self::CONSTRAINTS,
            )
            .context("h264")?;
        let fps = video.fps.unwrap_or(FALLBACK_FPS);
        let keyint = ((fps * self.keyint).round() as u32).max(1);

        let mut args: Vec<String> = [
            "-vf",
            &resize.filter(),
            "-c:v",
            "libx264",
            "-profile:v",
            self.profile,
            "-preset",
            self.preset,
            "-pix_fmt",
            "yuv420p",
        ]
        .map(String::from)
        .to_vec();
        let rate = match self.rate {
            Rate::Crf(crf) => {
                args.extend(["-crf".into(), crf.to_string()]);
                format!("crf {crf}")
            }
            Rate::Bitrate(bitrate) => {
                args.extend(["-b:v".into(), bitrate.to_string()]);
                format!("{} kbit/s", bitrate as f64 / 1000.0)
            }
        };
        args.extend(["-g".into(), keyint.to_string()]);

        Ok(VideoPlan {
            index: video.index,
            encoder: "libx264",
            label: format!(
                "H.264 {} {}x{}, {rate}, keyframe every {keyint} frames",
                self.profile, resize.width, resize.height
            ),
            args,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn source() -> probe::Video {
        probe::Video {
            index: 0,
            display_width: 1920.0,
            display_height: 1080.0,
            fps: Some(30000.0 / 1001.0),
        }
    }

    fn args(text: &str) -> Vec<String> {
        parse(text).unwrap().plan(&source()).unwrap().args
    }

    fn has(args: &[String], pair: [&str; 2]) -> bool {
        args.windows(2).any(|w| w[0] == pair[0] && w[1] == pair[1])
    }

    #[test]
    fn defaults() {
        let a = args("h264");
        assert!(has(&a, ["-vf", "scale=640:360,setsar=1"]));
        assert!(has(&a, ["-profile:v", "high"]));
        assert!(has(&a, ["-preset", "medium"]));
        assert!(has(&a, ["-crf", "23"]));
        assert!(has(&a, ["-g", "60"]));
    }

    #[test]
    fn options() {
        let a = args("h264,profile=baseline,bitrate=1.5M,keyint=0.5,preset=fast");
        assert!(has(&a, ["-profile:v", "baseline"]));
        assert!(has(&a, ["-b:v", "1500000"]));
        assert!(!a.contains(&"-crf".to_string()));
        assert!(has(&a, ["-g", "15"]));
        assert!(has(&a, ["-preset", "fast"]));
    }

    #[test]
    fn errors() {
        assert!(parse("h265").is_err());
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
