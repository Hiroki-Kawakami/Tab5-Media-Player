// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::Result;

use crate::spec::{Spec, one_of, yes_no};

pub const KEYS: [&str; 3] = ["rotate", "rotatewhen", "rotatemeta"];

#[derive(Clone, Copy, PartialEq, Debug)]
enum When {
    Landscape,
    Portrait,
    Always,
}

pub struct RotationSpec {
    degrees: u32,
    when: When,
    metadata: bool,
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Rotation {
    pub degrees: u32,
    pub display_rotation: Option<i32>,
}

fn degrees(value: &str) -> Result<u32> {
    let degrees: i32 = value
        .parse()
        .ok()
        .filter(|d: &i32| d % 90 == 0 && d.abs() <= 270)
        .ok_or_else(|| anyhow::anyhow!("expected 0, 90, 180, 270, -90, -180 or -270"))?;
    Ok(degrees.rem_euclid(360) as u32)
}

impl RotationSpec {
    pub fn take(spec: &mut Spec) -> Result<Self> {
        let degrees = spec.take("rotate", degrees)?.unwrap_or(90);
        let when = match spec.take("rotatewhen", |v| {
            one_of(v, &["landscape", "portrait", "always"])
        })? {
            Some("portrait") => When::Portrait,
            Some("always") => When::Always,
            _ => When::Landscape,
        };
        let metadata = spec.take("rotatemeta", yes_no)?.unwrap_or(true);
        Ok(Self {
            degrees,
            when,
            metadata,
        })
    }

    pub fn resolve(&self, width: u32, height: u32) -> Option<Rotation> {
        let applies = match self.when {
            When::Landscape => width > height,
            When::Portrait => height > width,
            When::Always => true,
        };
        if !applies || self.degrees == 0 {
            return None;
        }
        let display_rotation = self.metadata.then_some(match self.degrees {
            90 => -90,
            270 => 90,
            _ => 180,
        });
        Some(Rotation {
            degrees: self.degrees,
            display_rotation,
        })
    }
}

impl Rotation {
    pub fn stored(&self, width: u32, height: u32) -> (u32, u32) {
        if self.degrees == 180 {
            (width, height)
        } else {
            (height, width)
        }
    }

    pub fn label(&self) -> String {
        match self.display_rotation {
            Some(meta) => format!("rotated {}, metadata {meta}", self.degrees),
            None => format!("rotated {}, no metadata", self.degrees),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rotation(options: &str, width: u32, height: u32) -> Option<Rotation> {
        let mut spec = Spec::parse(&format!("mjpeg{options}")).unwrap();
        let rotation = RotationSpec::take(&mut spec).unwrap();
        spec.finish(&KEYS).unwrap();
        rotation.resolve(width, height)
    }

    #[test]
    fn default_turns_landscape_counter_clockwise() {
        let r = rotation("", 1280, 720).unwrap();
        assert_eq!(r.degrees, 90);
        assert_eq!(r.display_rotation, Some(-90));
        assert_eq!(r.stored(1280, 720), (720, 1280));
        assert_eq!(rotation("", 720, 1280), None);
        assert_eq!(rotation("", 720, 720), None);
    }

    #[test]
    fn angles() {
        let r = rotation(",rotate=-90", 1280, 720).unwrap();
        assert_eq!((r.degrees, r.display_rotation), (270, Some(90)));
        let r = rotation(",rotate=270", 1280, 720).unwrap();
        assert_eq!(r.display_rotation, Some(90));
        let r = rotation(",rotate=180", 1280, 720).unwrap();
        assert_eq!((r.degrees, r.display_rotation), (180, Some(180)));
        assert_eq!(r.stored(1280, 720), (1280, 720));
        assert_eq!(rotation(",rotate=-180", 1280, 720).unwrap().degrees, 180);
        assert_eq!(rotation(",rotate=-270", 1280, 720).unwrap().degrees, 90);
        assert_eq!(rotation(",rotate=0", 1280, 720), None);
    }

    #[test]
    fn conditions_and_metadata() {
        assert_eq!(rotation(",rotatewhen=portrait", 1280, 720), None);
        assert!(rotation(",rotatewhen=portrait", 720, 1280).is_some());
        assert!(rotation(",rotatewhen=always", 720, 720).is_some());
        let r = rotation(",rotatemeta=no", 1280, 720).unwrap();
        assert_eq!((r.degrees, r.display_rotation), (90, None));
    }

    #[test]
    fn errors() {
        for bad in ["45", "360", "-360", "90.0", "left"] {
            let mut spec = Spec::parse(&format!("mjpeg,rotate={bad}")).unwrap();
            assert!(RotationSpec::take(&mut spec).is_err(), "{bad}");
        }
        let mut spec = Spec::parse("mjpeg,rotatewhen=sometimes").unwrap();
        assert!(RotationSpec::take(&mut spec).is_err());
        let mut spec = Spec::parse("mjpeg,rotatemeta=maybe").unwrap();
        assert!(RotationSpec::take(&mut spec).is_err());
    }
}
