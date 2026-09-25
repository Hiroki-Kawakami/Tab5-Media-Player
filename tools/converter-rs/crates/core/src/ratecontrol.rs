// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use crate::jpeg::SizeModel;
use crate::video::mjpeg::Settings as MjpegSettings;

const CALIBRATION_WEIGHT: f32 = 0.2;

#[derive(Clone, Copy, Debug)]
pub struct Decision {
    pub quality: u8,
    pub estimate: f32,
    pub raw_estimate: f32,
}

pub struct RateControl {
    quality: u8,
    min_quality: u8,
    drain: f32,
    buffer: f32,
    max_frame: f32,
    fullness: f32,
    calibration: f32,
    span: usize,
}

impl RateControl {
    pub fn new(settings: &MjpegSettings, fps: f64) -> Self {
        Self {
            quality: settings.quality,
            min_quality: settings.min_quality,
            drain: (settings.bitrate as f64 / 8.0 / fps) as f32,
            buffer: settings.buffer as f32,
            max_frame: settings.max_frame as f32,
            fullness: 0.0,
            calibration: 1.0,
            span: 1,
        }
    }

    fn fits(&self, window: &[(&SizeModel, usize)], quality: u8) -> bool {
        let mut fullness = self.fullness;
        let mut previous = self.span;
        for (i, &(model, span)) in window.iter().enumerate() {
            let bytes = model.bytes(quality) * self.calibration;
            if i == 0 && bytes > self.max_frame {
                return false;
            }
            fullness = (fullness - self.drain * previous as f32).max(0.0) + bytes;
            if fullness > self.buffer {
                return false;
            }
            previous = span;
        }
        true
    }

    /// `window` pairs each upcoming frame with the number of frame intervals
    /// it stays on screen.
    pub fn decide(&self, window: &[(&SizeModel, usize)]) -> Decision {
        let quality = if self.fits(window, self.quality) {
            self.quality
        } else {
            let (mut lo, mut hi) = (self.min_quality, self.quality - 1);
            let mut best = self.min_quality;
            while lo <= hi {
                let mid = lo + (hi - lo) / 2;
                if self.fits(window, mid) {
                    best = mid;
                    lo = mid + 1;
                } else if mid == 0 {
                    break;
                } else {
                    hi = mid - 1;
                }
            }
            best
        };
        let raw_estimate = window[0].0.bytes(quality);
        Decision {
            quality,
            estimate: raw_estimate * self.calibration,
            raw_estimate,
        }
    }

    pub fn commit(&mut self, estimate: f32, span: usize) {
        self.fullness =
            ((self.fullness - self.drain * self.span as f32).max(0.0) + estimate).min(self.buffer);
        self.span = span;
    }

    pub fn feedback(&mut self, estimate: f32, raw_estimate: f32, actual: usize, calibrate: bool) {
        self.fullness = (self.fullness + actual as f32 - estimate).clamp(0.0, self.buffer);
        if calibrate && raw_estimate > 0.0 && actual > 0 {
            let ratio = actual as f32 / raw_estimate;
            self.calibration = (self.calibration.ln() * (1.0 - CALIBRATION_WEIGHT)
                + ratio.ln() * CALIBRATION_WEIGHT)
                .exp();
        }
    }
}

pub struct Bucket {
    drain: f64,
    buffer: f64,
    fullness: f64,
    pub peak: f64,
    pub overflows: usize,
    span: usize,
}

impl Bucket {
    pub fn new(settings: &MjpegSettings, fps: f64) -> Self {
        Self {
            drain: settings.bitrate as f64 / 8.0 / fps,
            buffer: settings.buffer as f64,
            fullness: 0.0,
            peak: 0.0,
            overflows: 0,
            span: 1,
        }
    }

    pub fn add(&mut self, bytes: usize, span: usize) {
        self.fullness = (self.fullness - self.drain * self.span as f64).max(0.0) + bytes as f64;
        self.span = span;
        self.peak = self.peak.max(self.fullness);
        if self.fullness > self.buffer {
            self.overflows += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::jpeg::HuffmanMode;

    fn settings(bitrate: u64) -> MjpegSettings {
        MjpegSettings {
            quality: 80,
            min_quality: 30,
            bitrate,
            buffer: 100_000,
            max_frame: 1 << 20,
            huffman: HuffmanMode::Optimal,
            dedup: true,
        }
    }

    fn model(level: i16) -> SizeModel {
        let blocks: Vec<[i16; 64]> = (0..3600)
            .map(|i| {
                std::array::from_fn(|n| ((i * 7 + n * 13) % 29) as i16 * level / (n as i16 + 1))
            })
            .collect();
        SizeModel::new(&blocks)
    }

    #[test]
    fn keeps_quality_when_the_bitrate_allows_it() {
        let rc = RateControl::new(&settings(1_000_000_000), 30.0);
        let m = model(40);
        let window: Vec<(&SizeModel, usize)> = std::iter::repeat_n((&m, 1), 30).collect();
        assert_eq!(rc.decide(&window).quality, 80);
    }

    #[test]
    fn lowers_quality_to_stay_under_the_bitrate() {
        let m = model(400);
        let per_frame_at_80 = m.bytes(80);
        let bitrate = (per_frame_at_80 * 0.5 * 8.0 * 30.0) as u64;
        let mut rc = RateControl::new(&settings(bitrate), 30.0);
        let window: Vec<(&SizeModel, usize)> = std::iter::repeat_n((&m, 1), 30).collect();
        let decision = rc.decide(&window);
        assert!(decision.quality < 80 && decision.quality >= 30);
        let mut bucket = Bucket::new(&settings(bitrate), 30.0);
        for _ in 0..90 {
            let d = rc.decide(&window);
            rc.commit(d.estimate, 1);
            bucket.add(d.estimate as usize, 1);
        }
        assert_eq!(bucket.overflows, 0);
    }

    #[test]
    fn recovers_after_an_unavoidable_overflow() {
        let heavy = model(1000);
        let light = model(4);
        let bitrate = (light.bytes(80) * 4.0 * 8.0 * 30.0) as u64;
        let mut rc = RateControl::new(&settings(bitrate), 30.0);
        let heavy_window: Vec<(&SizeModel, usize)> = std::iter::repeat_n((&heavy, 1), 30).collect();
        for _ in 0..60 {
            let d = rc.decide(&heavy_window);
            assert_eq!(d.quality, 30);
            rc.commit(d.estimate, 1);
        }
        let light_window: Vec<(&SizeModel, usize)> = std::iter::repeat_n((&light, 1), 30).collect();
        let mut recovered = None;
        for i in 0..30 {
            let d = rc.decide(&light_window);
            rc.commit(d.estimate, 1);
            if d.quality == 80 {
                recovered = Some(i);
                break;
            }
        }
        assert!(recovered.is_some_and(|i| i < 15), "{recovered:?}");
    }

    #[test]
    fn frames_shown_longer_drain_more() {
        let m = model(400);
        let bytes = m.bytes(80);
        let rc = RateControl::new(
            &MjpegSettings {
                buffer: (bytes * 4.0) as usize,
                ..settings((bytes * 0.5 * 8.0 * 30.0) as u64)
            },
            30.0,
        );
        let every_frame: Vec<(&SizeModel, usize)> = std::iter::repeat_n((&m, 1), 30).collect();
        assert!(rc.decide(&every_frame).quality < 80);
        let every_third: Vec<(&SizeModel, usize)> = std::iter::repeat_n((&m, 3), 10).collect();
        assert_eq!(rc.decide(&every_third).quality, 80);

        let mut bucket = Bucket::new(&settings(8 * 30 * 1000), 30.0);
        for _ in 0..10 {
            bucket.add(30_000, 30);
        }
        assert_eq!(bucket.overflows, 0);
    }

    #[test]
    fn calibration_follows_actual_sizes() {
        let mut rc = RateControl::new(&settings(1_000_000_000), 30.0);
        for _ in 0..50 {
            rc.feedback(1000.0, 1000.0, 1500, true);
        }
        assert!((rc.calibration - 1.5).abs() < 0.01);
    }

    #[test]
    fn bucket_counts_overflows() {
        let mut bucket = Bucket::new(&settings(8 * 30 * 1000), 30.0);
        for _ in 0..10 {
            bucket.add(1000, 1);
        }
        assert_eq!(bucket.overflows, 0);
        for _ in 0..10 {
            bucket.add(30_000, 1);
        }
        assert!(bucket.overflows > 0);
        assert!(bucket.peak > 100_000.0);
    }
}
