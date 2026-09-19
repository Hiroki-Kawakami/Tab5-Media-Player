// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use crate::spec::{Spec, positive_int};

pub const KEYS: [&str; 5] = ["width", "height", "long", "short", "scale"];

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
    pub max_width: u32,
    pub max_height: u32,
    pub max_macroblocks: Option<u32>,
    pub max_pixels: Option<u32>,
    pub default_long: u32,
    pub default_short: Option<u32>,
}

impl Constraints {
    pub fn check(&self, w: u32, h: u32) -> Result<()> {
        if self.exceeded_by(w, h) {
            bail!(
                "output {w}x{h} exceeds the player limit ({})",
                self.describe()
            );
        }
        Ok(())
    }

    fn exceeded_by(&self, w: u32, h: u32) -> bool {
        w > self.max_width
            || h > self.max_height
            || self
                .max_macroblocks
                .is_some_and(|max| w.div_ceil(16) * h.div_ceil(16) > max)
            || self.max_pixels.is_some_and(|max| w * h > max)
    }

    fn describe(&self) -> String {
        let mut parts = Vec::new();
        if self.max_width == self.max_height {
            parts.push(format!("each side up to {}", self.max_width));
        } else {
            if self.max_width != u32::MAX {
                parts.push(format!("width up to {}", self.max_width));
            }
            if self.max_height != u32::MAX {
                parts.push(format!("height up to {}", self.max_height));
            }
        }
        if let Some(max) = self.max_macroblocks {
            parts.push(format!("up to {max} macroblocks"));
        }
        if let Some(max) = self.max_pixels {
            parts.push(format!("up to {max} pixels"));
        }
        parts.join(", ")
    }
}

#[derive(Debug)]
pub struct Resize {
    pub width: u32,
    pub height: u32,
    pub scaled_width: u32,
    pub scaled_height: u32,
}

impl SizeSpec {
    pub fn take(spec: &mut Spec, constraints: &Constraints) -> Result<Self> {
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
                    long: Some(constraints.default_long),
                    short: constraints.default_short,
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

    pub fn fit(&self, source_width: f64, source_height: f64, constraints: &Constraints) -> Resize {
        let (box_width, box_height) = match self.target {
            Target::Axes { width, height } => (width, height),
            Target::Sides { long, short } if source_width >= source_height => (long, short),
            Target::Sides { long, short } => (short, long),
        };
        let align = |v: f64| {
            let unit = constraints.align;
            ((v / unit as f64).round() as u32 * unit).max(unit)
        };

        match self.mode {
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
        }
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
}

#[cfg(test)]
mod tests {
    use super::*;

    const H264: Constraints = Constraints {
        align: 2,
        max_width: 1280,
        max_height: 1280,
        max_macroblocks: Some(3600),
        max_pixels: None,
        default_long: 640,
        default_short: None,
    };

    const MJPEG: Constraints = Constraints {
        align: 2,
        max_width: 2560,
        max_height: u32::MAX,
        max_macroblocks: None,
        max_pixels: Some(1920 * 1088),
        default_long: 1280,
        default_short: Some(720),
    };

    fn resolve_with(
        constraints: &Constraints,
        options: &str,
        width: f64,
        height: f64,
    ) -> Result<Resize> {
        let mut spec = Spec::parse(&format!("h264{options}"))?;
        let size = SizeSpec::take(&mut spec, constraints)?;
        spec.finish(&KEYS)?;
        let resize = size.fit(width, height, constraints);
        constraints.check(resize.width, resize.height)?;
        Ok(resize)
    }

    fn resolve(options: &str, width: f64, height: f64) -> Result<Resize> {
        resolve_with(&H264, options, width, height)
    }

    #[test]
    fn mjpeg_defaults_to_the_panel_and_its_own_limits() {
        let size = |o, w, h| {
            let r = resolve_with(&MJPEG, o, w, h).unwrap();
            (r.width, r.height)
        };
        assert_eq!(size("", 1920.0, 1080.0), (1280, 720));
        assert_eq!(size("", 1080.0, 1920.0), (720, 1280));
        assert_eq!(size("", 1440.0, 1080.0), (960, 720));
        assert_eq!(size(",long=1920", 3840.0, 2160.0), (1920, 1080));
        let err = resolve_with(&MJPEG, ",long=2560", 3840.0, 2160.0).unwrap_err();
        assert!(
            err.to_string()
                .contains("width up to 2560, up to 2088960 pixels"),
            "{err}"
        );
    }

    fn scaled(r: &Resize) -> (u32, u32, u32, u32) {
        (r.scaled_width, r.scaled_height, r.width, r.height)
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
        assert_eq!(scaled(&r), (1280, 720, 720, 720));
        let r = resolve(",width=720,height=720,scale=cover", 1080.0, 1920.0).unwrap();
        assert_eq!(scaled(&r), (720, 1280, 720, 720));
    }

    #[test]
    fn cover_upscales() {
        let r = resolve(",long=1280,short=720,scale=cover", 320.0, 240.0).unwrap();
        assert_eq!(scaled(&r), (1280, 960, 1280, 720));
    }

    #[test]
    fn stretch_ignores_aspect() {
        let r = resolve(",width=720,height=720,scale=stretch", 1920.0, 1080.0).unwrap();
        assert_eq!(scaled(&r), (720, 720, 720, 720));
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
