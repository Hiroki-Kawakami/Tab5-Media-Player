// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result, bail};

use crate::spec::Spec;

pub const KEYS: [&str; 2] = ["fps", "maxfps"];
const DEFAULT_MAX: Rate = Rate { num: 30, den: 1 };
const NTSC_RATES: [(&str, u64); 3] = [("23.976", 24000), ("29.97", 30000), ("59.94", 60000)];
const TOLERANCE: f64 = 1e-3;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rate {
    num: u64,
    den: u64,
}

impl Rate {
    fn parse(value: &str) -> Result<Self> {
        let rate = if let Some((num, den)) = NTSC_RATES
            .iter()
            .find(|(text, _)| *text == value)
            .map(|(_, num)| (*num, 1001))
        {
            Self { num, den }
        } else if let Some((num, den)) = value.split_once('/') {
            Self {
                num: num
                    .parse()
                    .context("expected a rate such as 30, 12.5 or 30000/1001")?,
                den: den
                    .parse()
                    .context("expected a rate such as 30, 12.5 or 30000/1001")?,
            }
        } else {
            let (int_part, frac_part) = value.split_once('.').unwrap_or((value, ""));
            let digits = format!("{int_part}{frac_part}");
            if digits.is_empty() || !digits.chars().all(|c| c.is_ascii_digit()) {
                bail!("expected a rate such as 30, 12.5 or 30000/1001");
            }
            Self {
                num: digits.parse().context("too large")?,
                den: 10u64
                    .checked_pow(frac_part.len() as u32)
                    .context("too many decimals")?,
            }
        };
        if rate.num == 0 || rate.den == 0 {
            bail!("must be greater than 0");
        }
        Ok(rate.reduced())
    }

    fn reduced(self) -> Self {
        let (mut a, mut b) = (self.num, self.den);
        while b != 0 {
            (a, b) = (b, a % b);
        }
        Self {
            num: self.num / a,
            den: self.den / a,
        }
    }

    fn as_f64(self) -> f64 {
        self.num as f64 / self.den as f64
    }

    fn filter_value(self) -> String {
        if self.den == 1 {
            self.num.to_string()
        } else {
            format!("{}/{}", self.num, self.den)
        }
    }
}

fn describe(fps: f64) -> String {
    let text = format!("{fps:.3}");
    text.trim_end_matches('0').trim_end_matches('.').to_string()
}

#[derive(Clone, Copy)]
enum Mode {
    Exact(Rate),
    Max(Rate),
}

pub struct FrameRateSpec(Mode);

pub struct FrameRate {
    rate: Option<Rate>,
    pub fps: f64,
    pub label: String,
}

impl FrameRateSpec {
    pub fn take(spec: &mut Spec) -> Result<Self> {
        let fps = spec.take("fps", Rate::parse)?;
        let maxfps = spec.take("maxfps", Rate::parse)?;
        Ok(Self(match (fps, maxfps) {
            (Some(_), Some(_)) => bail!("{}: fps and maxfps cannot be combined", spec.codec),
            (Some(rate), None) => Mode::Exact(rate),
            (None, max) => Mode::Max(max.unwrap_or(DEFAULT_MAX)),
        }))
    }

    pub fn resolve(&self, source: Option<f64>) -> FrameRate {
        let rate = match self.0 {
            Mode::Exact(rate) => Some(rate),
            Mode::Max(max) => match source {
                Some(fps) if fps <= max.as_f64() + TOLERANCE => None,
                _ => Some(max),
            },
        };
        let fps = rate
            .map(Rate::as_f64)
            .or(source)
            .unwrap_or(DEFAULT_MAX.as_f64());
        let label = match source {
            Some(src) if (fps - src).abs() > TOLERANCE => {
                format!("{} fps (from {})", describe(fps), describe(src))
            }
            _ => format!("{} fps", describe(fps)),
        };
        FrameRate { rate, fps, label }
    }
}

impl FrameRate {
    pub fn filter(&self) -> Option<String> {
        self.rate.map(|r| format!("fps={}", r.filter_value()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rate(value: &str) -> Result<Rate> {
        Rate::parse(value)
    }

    fn resolve(options: &str, source: Option<f64>) -> FrameRate {
        let mut spec = Spec::parse(&format!("h264{options}")).unwrap();
        let framerate = FrameRateSpec::take(&mut spec).unwrap();
        spec.finish(&KEYS).unwrap();
        framerate.resolve(source)
    }

    #[test]
    fn parses_rates() {
        assert_eq!(rate("30").unwrap(), Rate { num: 30, den: 1 });
        assert_eq!(rate("12.5").unwrap(), Rate { num: 25, den: 2 });
        assert_eq!(
            rate("29.97").unwrap(),
            Rate {
                num: 30000,
                den: 1001
            }
        );
        assert_eq!(
            rate("23.976").unwrap(),
            Rate {
                num: 24000,
                den: 1001
            }
        );
        assert_eq!(
            rate("59.94").unwrap(),
            Rate {
                num: 60000,
                den: 1001
            }
        );
        assert_eq!(
            rate("60000/2002").unwrap(),
            Rate {
                num: 30000,
                den: 1001
            }
        );
        assert_eq!(rate("24.0").unwrap(), Rate { num: 24, den: 1 });
        for bad in ["0", "0/1", "30/0", "-30", "fast", "", "1.2.3", "30/"] {
            assert!(rate(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn default_caps_at_30() {
        let r = resolve("", Some(60.0));
        assert_eq!(r.filter().as_deref(), Some("fps=30"));
        assert_eq!(r.fps, 30.0);
        assert_eq!(r.label, "30 fps (from 60)");
        let r = resolve("", Some(30000.0 / 1001.0));
        assert_eq!(r.filter(), None);
        assert_eq!(r.label, "29.97 fps");
        let r = resolve("", Some(24.0));
        assert_eq!(r.filter(), None);
        assert_eq!(r.fps, 24.0);
    }

    #[test]
    fn maxfps_only_lowers() {
        let r = resolve(",maxfps=24", Some(60000.0 / 1001.0));
        assert_eq!(r.filter().as_deref(), Some("fps=24"));
        assert_eq!(r.label, "24 fps (from 59.94)");
        assert_eq!(resolve(",maxfps=60", Some(59.94)).filter(), None);
    }

    #[test]
    fn unknown_source_rate_is_capped() {
        let r = resolve("", None);
        assert_eq!(r.filter().as_deref(), Some("fps=30"));
        assert_eq!(r.fps, 30.0);
        assert_eq!(r.label, "30 fps");
    }

    #[test]
    fn fps_is_exact() {
        let r = resolve(",fps=29.97", Some(15.0));
        assert_eq!(r.filter().as_deref(), Some("fps=30000/1001"));
        assert_eq!(r.label, "29.97 fps (from 15)");
        let r = resolve(",fps=30", Some(30.0));
        assert_eq!(r.filter().as_deref(), Some("fps=30"));
        assert_eq!(r.label, "30 fps");
    }

    #[test]
    fn fps_and_maxfps_conflict() {
        let mut spec = Spec::parse("h264,fps=30,maxfps=30").unwrap();
        assert!(FrameRateSpec::take(&mut spec).is_err());
    }
}
