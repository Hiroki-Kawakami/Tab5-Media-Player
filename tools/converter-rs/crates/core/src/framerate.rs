// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::fmt;

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
    pub fn new(num: u64, den: u64) -> Option<Self> {
        (num > 0 && den > 0).then(|| Self { num, den }.reduced())
    }

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

    pub fn num(self) -> u64 {
        self.num
    }

    pub fn den(self) -> u64 {
        self.den
    }

    pub fn as_f64(self) -> f64 {
        self.num as f64 / self.den as f64
    }
}

impl fmt::Display for Rate {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        if self.den == 1 {
            write!(f, "{}", self.num)
        } else {
            write!(f, "{}/{}", self.num, self.den)
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
    pub convert: Option<Rate>,
    pub rate: Rate,
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

    pub fn resolve(&self, source: Option<Rate>) -> FrameRate {
        let convert = match self.0 {
            Mode::Exact(rate) => Some(rate),
            Mode::Max(max) => match source {
                Some(src) if src.as_f64() <= max.as_f64() + TOLERANCE => None,
                _ => Some(max),
            },
        };
        let rate = convert.or(source).unwrap_or(DEFAULT_MAX);
        let label = match source {
            Some(src) if (rate.as_f64() - src.as_f64()).abs() > TOLERANCE => format!(
                "{} fps (from {})",
                describe(rate.as_f64()),
                describe(src.as_f64())
            ),
            _ => format!("{} fps", describe(rate.as_f64())),
        };
        FrameRate {
            convert,
            rate,
            label,
        }
    }
}

pub struct FrameSelector {
    rate: Rate,
    start: Option<i64>,
    next: i64,
}

impl FrameSelector {
    pub fn new(rate: Rate) -> Self {
        Self {
            rate,
            start: None,
            next: 0,
        }
    }

    fn slot(&self, start: i64, time_us: i64) -> i64 {
        let ticks = (time_us - start) as i128 * self.rate.num as i128;
        let unit = self.rate.den as i128 * 1_000_000;
        ((ticks * 2 + unit) / (unit * 2)) as i64
    }

    pub fn push(&mut self, pts_us: i64) -> u64 {
        let Some(start) = self.start else {
            self.start = Some(pts_us);
            return 0;
        };
        let slot = self.slot(start, pts_us).max(self.next);
        let repeats = slot - self.next;
        self.next = slot;
        repeats as u64
    }

    pub fn finish(&mut self, end_us: i64) -> u64 {
        let Some(start) = self.start else {
            return 0;
        };
        let end = self.slot(start, end_us).max(self.next + 1);
        let repeats = end - self.next;
        self.next = end;
        repeats as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rate(value: &str) -> Result<Rate> {
        Rate::parse(value)
    }

    fn r(num: u64, den: u64) -> Option<Rate> {
        Rate::new(num, den)
    }

    fn resolve(options: &str, source: Option<Rate>) -> FrameRate {
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
        let fr = resolve("", r(60, 1));
        assert_eq!(fr.convert, r(30, 1));
        assert_eq!(fr.rate.as_f64(), 30.0);
        assert_eq!(fr.label, "30 fps (from 60)");
        let fr = resolve("", r(30000, 1001));
        assert_eq!(fr.convert, None);
        assert_eq!(fr.rate.to_string(), "30000/1001");
        assert_eq!(fr.label, "29.97 fps");
        let fr = resolve("", r(24, 1));
        assert_eq!(fr.convert, None);
        assert_eq!(fr.rate.as_f64(), 24.0);
    }

    #[test]
    fn maxfps_only_lowers() {
        let fr = resolve(",maxfps=24", r(60000, 1001));
        assert_eq!(fr.convert, r(24, 1));
        assert_eq!(fr.label, "24 fps (from 59.94)");
        assert_eq!(resolve(",maxfps=60", r(60000, 1001)).convert, None);
    }

    #[test]
    fn unknown_source_rate_is_capped() {
        let fr = resolve("", None);
        assert_eq!(fr.convert, r(30, 1));
        assert_eq!(fr.rate.as_f64(), 30.0);
        assert_eq!(fr.label, "30 fps");
    }

    #[test]
    fn fps_is_exact() {
        let fr = resolve(",fps=29.97", r(15, 1));
        assert_eq!(fr.convert, r(30000, 1001));
        assert_eq!(fr.label, "29.97 fps (from 15)");
        let fr = resolve(",fps=30", r(30, 1));
        assert_eq!(fr.convert, r(30, 1));
        assert_eq!(fr.label, "30 fps");
    }

    fn select(rate: Rate, pts: &[i64], end: i64) -> Vec<u64> {
        let mut selector = FrameSelector::new(rate);
        let mut repeats = Vec::new();
        for (i, &t) in pts.iter().enumerate() {
            let previous = selector.push(t);
            if i > 0 {
                repeats.push(previous);
            }
        }
        repeats.push(selector.finish(end));
        repeats
    }

    #[test]
    fn frame_selection_drops_and_repeats() {
        let frames = |fps: i64, n: i64| (0..n).map(|i| i * 1_000_000 / fps).collect::<Vec<_>>();
        let halved = select(r(30, 1).unwrap(), &frames(60, 60), 1_000_000);
        assert_eq!(halved.iter().sum::<u64>(), 30);
        assert!(halved.iter().all(|&c| c <= 1));
        let same = select(r(30000, 1001).unwrap(), &frames(30, 30), 1_000_000);
        assert_eq!(same.iter().sum::<u64>(), 30);
        let doubled = select(r(30, 1).unwrap(), &frames(15, 15), 1_000_000);
        assert_eq!(doubled, vec![2; 15]);
        let offset = select(r(10, 1).unwrap(), &[500_000, 600_000, 700_000], 800_000);
        assert_eq!(offset, [1, 1, 1]);
    }

    #[test]
    fn fps_and_maxfps_conflict() {
        let mut spec = Spec::parse("h264,fps=30,maxfps=30").unwrap();
        assert!(FrameRateSpec::take(&mut spec).is_err());
    }
}
