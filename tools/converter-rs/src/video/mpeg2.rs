// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::{DECODER_LIMITS, PictureSpec, VideoOutput, VideoPlan};
use crate::probe;
use crate::spec::{Spec, int_in, one_of, quantity, seconds, yes_no};

pub const KEYS: [&str; 6] = ["qscale", "bitrate", "bframes", "keyint", "gop", "hq"];
const MAX_BFRAMES: u32 = 3;
const DEFAULT_QSCALE: u32 = 8;

enum Rate {
    Qscale(u32),
    Bitrate(u64),
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

    pub fn plan(&self, video: &probe::Video) -> Result<VideoPlan> {
        let picture = self.picture.resolve("mpeg2", video, false, "", None)?;
        let keyint = picture.keyint(self.keyint);

        let mut args: Vec<String> = [
            "-vf",
            &picture.filter,
            "-c:v",
            "mpeg2video",
            "-pix_fmt",
            "yuv420p",
        ]
        .map(String::from)
        .to_vec();
        let rate = match self.rate {
            Rate::Qscale(q) => {
                args.extend(["-q:v".into(), q.to_string()]);
                format!("qscale {q}")
            }
            Rate::Bitrate(bitrate) => {
                args.extend(["-b:v".into(), bitrate.to_string()]);
                format!("{} kbit/s", bitrate as f64 / 1000.0)
            }
        };
        args.extend([
            "-bf".into(),
            self.bframes.to_string(),
            "-g".into(),
            keyint.to_string(),
        ]);
        if self.closed_gop {
            args.extend(["-flags", "+cgop", "-sc_threshold", "1000000000"].map(String::from));
        }
        if self.hq {
            args.extend(["-mbd", "rd", "-trellis", "1", "-intra_vlc", "1"].map(String::from));
        }

        Ok(VideoPlan {
            index: video.index,
            encoder: Some("mpeg2video"),
            label: format!(
                "MPEG-2 {}, {rate}, {} B pictures, {} GOP of {keyint} frames{}",
                picture.label,
                self.bframes,
                if self.closed_gop { "closed" } else { "open" },
                if self.hq { ", hq" } else { "" },
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
        let a = args("mpeg2");
        assert!(has(&a, ["-vf", "scale=640:360,setsar=1"]));
        assert!(has(&a, ["-c:v", "mpeg2video"]));
        assert!(has(&a, ["-pix_fmt", "yuv420p"]));
        assert!(has(&a, ["-q:v", "8"]));
        assert!(has(&a, ["-bf", "2"]));
        assert!(has(&a, ["-g", "60"]));
        assert!(has(&a, ["-flags", "+cgop"]));
        assert!(has(&a, ["-sc_threshold", "1000000000"]));
        assert!(has(&a, ["-mbd", "rd"]));
        assert!(has(&a, ["-trellis", "1"]));
        assert!(has(&a, ["-intra_vlc", "1"]));
    }

    #[test]
    fn options() {
        let a = args("mpeg2,bitrate=2M,bframes=0,keyint=1,gop=open,hq=no,short=720");
        assert!(has(&a, ["-vf", "scale=1280:720,setsar=1"]));
        assert!(has(&a, ["-b:v", "2000000"]));
        assert!(!a.contains(&"-q:v".to_string()));
        assert!(has(&a, ["-bf", "0"]));
        assert!(has(&a, ["-g", "30"]));
        assert!(!a.contains(&"+cgop".to_string()));
        assert!(!a.contains(&"-sc_threshold".to_string()));
        assert!(!a.contains(&"-mbd".to_string()));
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
