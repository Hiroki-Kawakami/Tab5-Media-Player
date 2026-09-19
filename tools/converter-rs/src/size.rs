// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use crate::spec::{Spec, positive_int};

pub const KEYS: [&str; 5] = ["width", "height", "long", "short", "scale"];
const DEFAULT_LONG_SIDE: u32 = 640;

#[derive(Clone, Copy, PartialEq)]
enum ScaleMode {
    Contain,
    Fit,
    Cover,
    Stretch,
}

enum Target {
    Axes {
        width: Option<u32>,
        height: Option<u32>,
    },
    Sides {
        long: Option<u32>,
        short: Option<u32>,
    },
}

pub struct SizeSpec {
    target: Target,
    mode: ScaleMode,
}

pub struct Constraints {
    pub align: u32,
    pub max_side: u32,
    pub max_macroblocks: u32,
}

#[derive(Debug)]
pub struct Resize {
    pub width: u32,
    pub height: u32,
    scaled_width: u32,
    scaled_height: u32,
}

impl SizeSpec {
    pub fn take(spec: &mut Spec) -> Result<Self> {
        let width = spec.take("width", positive_int)?;
        let height = spec.take("height", positive_int)?;
        let long = spec.take("long", positive_int)?;
        let short = spec.take("short", positive_int)?;
        let mode = spec
            .take("scale", |v| {
                Ok(match v {
                    "contain" => ScaleMode::Contain,
                    "fit" => ScaleMode::Fit,
                    "cover" => ScaleMode::Cover,
                    "stretch" => ScaleMode::Stretch,
                    _ => bail!("expected contain, fit, cover or stretch"),
                })
            })?
            .unwrap_or(ScaleMode::Contain);

        let axes = width.is_some() || height.is_some();
        let sides = long.is_some() || short.is_some();
        let (target, both) = match (axes, sides) {
            (true, true) => bail!(
                "{}: width/height cannot be combined with long/short",
                spec.codec
            ),
            (true, false) => (
                Target::Axes { width, height },
                width.is_some() && height.is_some(),
            ),
            (false, true) => (
                Target::Sides { long, short },
                long.is_some() && short.is_some(),
            ),
            (false, false) => (
                Target::Sides {
                    long: Some(DEFAULT_LONG_SIDE),
                    short: None,
                },
                false,
            ),
        };
        if matches!(mode, ScaleMode::Cover | ScaleMode::Stretch) && !both {
            bail!(
                "{}: scale=cover and scale=stretch need both width and height, or both long and short",
                spec.codec
            );
        }
        Ok(Self { target, mode })
    }

    pub fn resolve(
        &self,
        source_width: f64,
        source_height: f64,
        constraints: &Constraints,
    ) -> Result<Resize> {
        let (box_width, box_height) = match self.target {
            Target::Axes { width, height } => (width, height),
            Target::Sides { long, short } if source_width >= source_height => (long, short),
            Target::Sides { long, short } => (short, long),
        };
        let align = |v: f64| {
            let unit = constraints.align;
            ((v / unit as f64).round() as u32 * unit).max(unit)
        };

        let resize = match self.mode {
            ScaleMode::Contain | ScaleMode::Fit => {
                let mut factor = f64::INFINITY;
                if let Some(w) = box_width {
                    factor = factor.min(w as f64 / source_width);
                }
                if let Some(h) = box_height {
                    factor = factor.min(h as f64 / source_height);
                }
                if self.mode == ScaleMode::Contain {
                    factor = factor.min(1.0);
                }
                Resize::plain(align(source_width * factor), align(source_height * factor))
            }
            ScaleMode::Stretch => Resize::plain(
                align(box_width.unwrap_or_default() as f64),
                align(box_height.unwrap_or_default() as f64),
            ),
            ScaleMode::Cover => {
                let width = align(box_width.unwrap_or_default() as f64);
                let height = align(box_height.unwrap_or_default() as f64);
                let factor = (width as f64 / source_width).max(height as f64 / source_height);
                Resize {
                    width,
                    height,
                    scaled_width: align(source_width * factor).max(width),
                    scaled_height: align(source_height * factor).max(height),
                }
            }
        };

        let (w, h) = (resize.width, resize.height);
        if w > constraints.max_side
            || h > constraints.max_side
            || w.div_ceil(16) * h.div_ceil(16) > constraints.max_macroblocks
        {
            bail!(
                "output {w}x{h} exceeds the player limit (each side up to {}, up to {} macroblocks)",
                constraints.max_side,
                constraints.max_macroblocks
            );
        }
        Ok(resize)
    }
}

impl Resize {
    fn plain(width: u32, height: u32) -> Self {
        Self {
            width,
            height,
            scaled_width: width,
            scaled_height: height,
        }
    }

    pub fn filter(&self) -> String {
        let mut filter = format!(
            "scale={}:{},setsar=1",
            self.scaled_width, self.scaled_height
        );
        if (self.scaled_width, self.scaled_height) != (self.width, self.height) {
            filter.push_str(&format!(",crop={}:{}", self.width, self.height));
        }
        filter
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const H264: Constraints = Constraints {
        align: 2,
        max_side: 1280,
        max_macroblocks: 3600,
    };

    fn resolve(options: &str, width: f64, height: f64) -> Result<Resize> {
        let mut spec = Spec::parse(&format!("h264{options}"))?;
        let size = SizeSpec::take(&mut spec)?;
        spec.finish(&KEYS)?;
        size.resolve(width, height, &H264)
    }

    fn size(options: &str, width: f64, height: f64) -> (u32, u32) {
        let r = resolve(options, width, height).unwrap();
        (r.width, r.height)
    }

    #[test]
    fn default_is_long_640_contain() {
        assert_eq!(size("", 1920.0, 1080.0), (640, 360));
        assert_eq!(size("", 1080.0, 1920.0), (360, 640));
        assert_eq!(size("", 320.0, 240.0), (320, 240));
    }

    #[test]
    fn long_and_short_follow_orientation() {
        let opts = ",long=1280,short=720";
        assert_eq!(size(opts, 1920.0, 1080.0), (1280, 720));
        assert_eq!(size(opts, 1080.0, 1920.0), (720, 1280));
        assert_eq!(size(",short=720", 1920.0, 1080.0), (1280, 720));
        assert_eq!(size(",short=720", 1080.0, 1920.0), (720, 1280));
    }

    #[test]
    fn one_axis_keeps_aspect() {
        assert_eq!(size(",width=720", 1920.0, 1080.0), (720, 406));
        assert_eq!(size(",width=720", 1080.0, 1920.0), (720, 1280));
        assert_eq!(size(",height=720", 1920.0, 1080.0), (1280, 720));
        assert_eq!(size(",height=720", 1080.0, 1920.0), (406, 720));
    }

    #[test]
    fn contain_fits_inside_the_box() {
        let opts = ",width=720,height=720";
        assert_eq!(size(opts, 1920.0, 1080.0), (720, 406));
        assert_eq!(size(opts, 1080.0, 1920.0), (406, 720));
    }

    #[test]
    fn contain_never_upscales_but_fit_does() {
        assert_eq!(size(",short=720", 640.0, 360.0), (640, 360));
        assert_eq!(size(",short=720,scale=fit", 640.0, 360.0), (1280, 720));
    }

    #[test]
    fn cover_crops_to_the_box() {
        let r = resolve(",width=720,height=720,scale=cover", 1920.0, 1080.0).unwrap();
        assert_eq!((r.width, r.height), (720, 720));
        assert_eq!(r.filter(), "scale=1280:720,setsar=1,crop=720:720");
        let r = resolve(",width=720,height=720,scale=cover", 1080.0, 1920.0).unwrap();
        assert_eq!(r.filter(), "scale=720:1280,setsar=1,crop=720:720");
    }

    #[test]
    fn cover_upscales() {
        let r = resolve(",long=1280,short=720,scale=cover", 320.0, 240.0).unwrap();
        assert_eq!(r.filter(), "scale=1280:960,setsar=1,crop=1280:720");
    }

    #[test]
    fn stretch_ignores_aspect() {
        let r = resolve(",width=720,height=720,scale=stretch", 1920.0, 1080.0).unwrap();
        assert_eq!(r.filter(), "scale=720:720,setsar=1");
    }

    #[test]
    fn odd_sizes_are_aligned() {
        assert_eq!(size(",width=721", 1920.0, 1080.0), (722, 406));
        assert_eq!(size(",long=1280", 853.33, 480.0), (854, 480));
    }

    #[test]
    fn player_limits_are_errors() {
        let err = resolve(",width=1280,height=1280", 1440.0, 1080.0).unwrap_err();
        assert!(err.to_string().contains("1280x960"), "{err}");
        assert!(resolve(",long=1920", 3840.0, 2160.0).is_err());
        assert!(resolve(",width=1280,height=1280,scale=stretch", 100.0, 100.0).is_err());
    }

    #[test]
    fn invalid_combinations() {
        assert!(resolve(",width=720,long=1280", 1920.0, 1080.0).is_err());
        assert!(resolve(",width=720,scale=cover", 1920.0, 1080.0).is_err());
        assert!(resolve(",scale=stretch", 1920.0, 1080.0).is_err());
        assert!(resolve(",scale=zoom", 1920.0, 1080.0).is_err());
        assert!(resolve(",width=0", 1920.0, 1080.0).is_err());
    }
}
