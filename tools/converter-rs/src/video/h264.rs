// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::{DECODER_LIMITS, PictureSpec, VideoOutput, VideoPlan};
use crate::probe;
use crate::spec::{Spec, int_in, one_of, quantity, seconds};

pub const KEYS: [&str; 5] = ["profile", "crf", "bitrate", "keyint", "preset"];
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

enum Rate {
    Crf(u32),
    Bitrate(u64),
}

pub struct H264 {
    picture: PictureSpec,
    profile: &'static str,
    rate: Rate,
    keyint: f64,
    preset: &'static str,
}

impl H264 {
    pub fn take(spec: &mut Spec) -> Result<Self> {
        let picture = PictureSpec::take(spec, &DECODER_LIMITS)?;
        let profile = spec
            .take("profile", |v| one_of(v, &PROFILES))?
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

    pub fn plan(&self, video: &probe::Video) -> Result<VideoPlan> {
        let picture = self.picture.resolve("h264", video, false, "")?;
        let keyint = picture.keyint(self.keyint);

        let mut args: Vec<String> = [
            "-vf",
            &picture.filter,
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
            encoder: Some("libx264"),
            label: format!(
                "H.264 {} {}, {rate}, keyframe every {keyint} frames",
                self.profile, picture.label
            ),
            output: VideoOutput::Ffmpeg(args),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::parse;
    use super::super::tests::{args, has, source};

    #[test]
    fn defaults() {
        let a = args("h264");
        assert!(has(&a, ["-vf", "scale=640:360,setsar=1"]));
        assert!(has(&a, ["-c:v", "libx264"]));
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
