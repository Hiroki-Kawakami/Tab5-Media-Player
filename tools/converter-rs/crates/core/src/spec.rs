// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result, bail};

pub struct Spec {
    pub codec: String,
    options: Vec<(String, String)>,
}

impl Spec {
    pub fn parse(text: &str) -> Result<Self> {
        let mut parts = text.split(',');
        let codec = parts.next().unwrap_or_default().trim().to_string();
        if codec.is_empty() {
            bail!("'{text}': codec name is missing");
        }
        let mut options: Vec<(String, String)> = Vec::new();
        for part in parts {
            let Some((key, value)) = part.split_once('=') else {
                bail!("{codec}: expected key=value, got '{part}'");
            };
            let (key, value) = (key.trim(), value.trim());
            if key.is_empty() || value.is_empty() {
                bail!("{codec}: expected key=value, got '{part}'");
            }
            if options.iter().any(|(k, _)| k == key) {
                bail!("{codec}: '{key}' is given more than once");
            }
            options.push((key.to_string(), value.to_string()));
        }
        Ok(Self { codec, options })
    }

    pub fn merged(self, over: Spec, exclusive: &[&[&[&str]]]) -> Spec {
        if self.codec != over.codec {
            return over;
        }
        let overridden = |key: &str| {
            over.options.iter().any(|(k, _)| {
                k == key
                    || exclusive.iter().any(|choices| {
                        let choice = |key: &str| choices.iter().position(|c| c.contains(&key));
                        matches!((choice(k), choice(key)), (Some(a), Some(b)) if a != b)
                    })
            })
        };
        let mut options: Vec<(String, String)> = self
            .options
            .into_iter()
            .filter(|(key, _)| !overridden(key))
            .collect();
        options.extend(over.options);
        Spec {
            codec: over.codec,
            options,
        }
    }

    pub fn to_text(&self) -> String {
        std::iter::once(self.codec.clone())
            .chain(self.options.iter().map(|(k, v)| format!("{k}={v}")))
            .collect::<Vec<_>>()
            .join(",")
    }

    pub fn take<T>(
        &mut self,
        key: &str,
        parse: impl FnOnce(&str) -> Result<T>,
    ) -> Result<Option<T>> {
        let Some(pos) = self.options.iter().position(|(k, _)| k == key) else {
            return Ok(None);
        };
        let (_, value) = self.options.remove(pos);
        parse(&value)
            .map(Some)
            .with_context(|| format!("{}: {key}={value}", self.codec))
    }

    pub fn finish(self, keys: &[&str]) -> Result<()> {
        if let Some((key, _)) = self.options.first() {
            if keys.is_empty() {
                bail!("{}: takes no options, got '{key}'", self.codec);
            }
            bail!(
                "{}: unknown key '{key}' (keys: {})",
                self.codec,
                keys.join(", ")
            );
        }
        Ok(())
    }
}

pub fn positive_int(value: &str) -> Result<u32> {
    let v: u32 = value.parse().context("not a whole number")?;
    if v == 0 {
        bail!("must be greater than 0");
    }
    Ok(v)
}

pub fn int_in(value: &str, min: u32, max: u32) -> Result<u32> {
    let v: u32 = value.parse().context("not a whole number")?;
    if !(min..=max).contains(&v) {
        bail!("must be between {min} and {max}");
    }
    Ok(v)
}

pub fn quantity(value: &str) -> Result<u64> {
    let (number, multiplier) = if let Some(n) = value.strip_suffix(['k', 'K']) {
        (n, 1_000)
    } else if let Some(n) = value.strip_suffix('M') {
        (n, 1_000_000)
    } else {
        (value, 1)
    };
    let (int_part, frac_part) = number.split_once('.').unwrap_or((number, ""));
    let digits = format!("{int_part}{frac_part}");
    let mantissa: u64 = digits
        .parse()
        .ok()
        .filter(|_| digits.chars().all(|c| c.is_ascii_digit()))
        .context("expected a number such as 800000, 128k, 44.1k or 1.5M")?;
    let divisor = 10u64
        .checked_pow(frac_part.len() as u32)
        .context("too many decimals")?;
    let scaled = mantissa.checked_mul(multiplier).context("too large")?;
    if scaled % divisor != 0 {
        bail!("not a whole number");
    }
    let v = scaled / divisor;
    if v == 0 {
        bail!("must be greater than 0");
    }
    Ok(v)
}

pub type Defaults = Vec<(&'static str, String)>;

pub fn quantity_text(value: u64) -> String {
    if value >= 1_000_000 && value.is_multiple_of(1000) {
        format!("{}M", value as f64 / 1e6)
    } else if value >= 1000 && value.is_multiple_of(1000) {
        format!("{}k", value / 1000)
    } else {
        value.to_string()
    }
}

pub fn seconds(value: &str) -> Result<f64> {
    let v: f64 = value.parse().context("not a number")?;
    if !(v.is_finite() && v > 0.0) {
        bail!("must be greater than 0");
    }
    Ok(v)
}

pub fn yes_no(value: &str) -> Result<bool> {
    match value {
        "yes" => Ok(true),
        "no" => Ok(false),
        _ => bail!("expected yes or no"),
    }
}

pub fn one_of<'a>(value: &str, choices: &[&'a str]) -> Result<&'a str> {
    choices
        .iter()
        .find(|c| **c == value)
        .copied()
        .with_context(|| format!("expected one of {}", choices.join(", ")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_codec_and_options() {
        let mut spec = Spec::parse("h264, crf=20 ,profile=main").unwrap();
        assert_eq!(spec.codec, "h264");
        assert_eq!(spec.take("crf", positive_int).unwrap(), Some(20));
        assert_eq!(spec.take("crf", positive_int).unwrap(), None);
        assert!(spec.finish(&["crf"]).is_err());
    }

    #[test]
    fn merges_over_the_same_codec_only() {
        let base = || Spec::parse("h264,long=640,maxfps=15").unwrap();
        let merge = |text| base().merged(Spec::parse(text).unwrap(), &[&[&["fps"], &["maxfps"]]]);
        assert_eq!(
            merge("h264,crf=20,maxfps=24").to_text(),
            "h264,long=640,crf=20,maxfps=24"
        );
        assert_eq!(merge("h264,fps=24").to_text(), "h264,long=640,fps=24");
        let replaced = merge("mjpeg,quality=90");
        assert_eq!(replaced.to_text(), "mjpeg,quality=90");
    }

    #[test]
    fn rejects_malformed_specs() {
        assert!(Spec::parse("").is_err());
        assert!(Spec::parse(",crf=20").is_err());
        assert!(Spec::parse("h264,crf").is_err());
        assert!(Spec::parse("h264,crf=").is_err());
        assert!(Spec::parse("h264,crf=20,crf=21").is_err());
    }

    #[test]
    fn unknown_key_lists_the_known_ones() {
        let spec = Spec::parse("h264,sise=640").unwrap();
        let err = spec.finish(&["size", "crf"]).unwrap_err().to_string();
        assert!(err.contains("'sise'") && err.contains("size, crf"), "{err}");
    }

    #[test]
    fn bad_value_names_codec_and_key() {
        let mut spec = Spec::parse("h264,crf=99").unwrap();
        let err = spec.take("crf", |v| int_in(v, 0, 51)).unwrap_err();
        assert_eq!(format!("{err:#}"), "h264: crf=99: must be between 0 and 51");
    }

    #[test]
    fn quantities() {
        assert_eq!(quantity("800000").unwrap(), 800_000);
        assert_eq!(quantity("128k").unwrap(), 128_000);
        assert_eq!(quantity("44.1k").unwrap(), 44_100);
        assert_eq!(quantity("11.025k").unwrap(), 11_025);
        assert_eq!(quantity("1.5M").unwrap(), 1_500_000);
        assert!(quantity("1.2345k").is_err());
        assert!(quantity("0").is_err());
        assert!(quantity("-5k").is_err());
        assert!(quantity("k").is_err());
        assert!(quantity("1.2.3k").is_err());
        assert!(quantity("fast").is_err());
        for (value, text) in [
            (800, "800"),
            (128_000, "128k"),
            (44_100, "44100"),
            (24_000_000, "24M"),
            (1_500_000, "1.5M"),
            (1_048_576, "1048576"),
        ] {
            assert_eq!(quantity_text(value), text);
            assert_eq!(quantity(text).unwrap(), value);
        }
    }
}
