// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::{BTreeMap, VecDeque};

use anyhow::{Result, bail};

use crate::framerate::Rate;
use crate::jpeg::{Encoded, SizeModel};
use crate::ratecontrol::{Bucket, Decision, RateControl};
use crate::video::mjpeg::{PLAYER_MAX_FRAME, Settings};

const LOOKAHEAD_SECONDS: f64 = 1.0;

pub const TIMESCALE: u32 = 1_200_000;

pub fn ticks(rate: Rate, frame: u64) -> u64 {
    let ticks = frame as u128 * TIMESCALE as u128 * rate.den() as u128;
    let num = rate.num() as u128;
    ((ticks * 2 + num) / (num * 2)) as u64
}

pub struct Reorder<T> {
    pending: BTreeMap<usize, T>,
    next: usize,
}

impl<T> Default for Reorder<T> {
    fn default() -> Self {
        Self {
            pending: BTreeMap::new(),
            next: 0,
        }
    }
}

impl<T> Reorder<T> {
    pub fn push(&mut self, index: usize, item: T) {
        self.pending.insert(index, item);
    }

    pub fn pop(&mut self) -> Option<T> {
        let item = self.pending.remove(&self.next)?;
        self.next += 1;
        Some(item)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Decided {
    pub index: usize,
    pub decision: Decision,
}

#[derive(Clone, Copy, Debug)]
pub struct Feedback {
    estimate: f32,
    raw_estimate: f32,
    actual: usize,
    calibrate: bool,
}

pub struct Scheduler {
    rate: RateControl,
    order: Reorder<SizeModel>,
    window: VecDeque<(usize, SizeModel)>,
    next: usize,
    lookahead: usize,
}

impl Scheduler {
    pub fn new(settings: &Settings, fps: f64) -> Self {
        Self {
            rate: RateControl::new(settings, fps),
            order: Reorder::default(),
            window: VecDeque::new(),
            next: 0,
            lookahead: ((fps * LOOKAHEAD_SECONDS).round() as usize).max(1),
        }
    }

    pub fn push(&mut self, index: usize, model: SizeModel) {
        self.order.push(index, model);
        while let Some(model) = self.order.pop() {
            self.window.push_back((self.next, model));
            self.next += 1;
        }
    }

    pub fn is_empty(&self) -> bool {
        self.window.is_empty()
    }

    pub fn decide(&mut self, finished: bool) -> Option<Decided> {
        let ready = self.window.len() > self.lookahead || (finished && !self.window.is_empty());
        if !ready {
            return None;
        }
        let models: Vec<&SizeModel> = self.window.iter().map(|(_, m)| m).collect();
        let decision = self.rate.decide(&models);
        self.rate.commit(decision.estimate);
        let (index, _) = self.window.pop_front().expect("window is not empty");
        Some(Decided { index, decision })
    }

    pub fn feedback(&mut self, f: &Feedback) {
        self.rate
            .feedback(f.estimate, f.raw_estimate, f.actual, f.calibrate);
    }
}

pub struct Tally {
    stats: Stats,
    bucket: Bucket,
    quality: u8,
}

impl Tally {
    pub fn new(settings: &Settings, fps: f64) -> Self {
        Self {
            stats: Stats::new(settings.quality),
            bucket: Bucket::new(settings, fps),
            quality: settings.quality,
        }
    }

    pub fn record(
        &mut self,
        index: usize,
        encoded: &Encoded,
        decided: &Decision,
    ) -> Result<Feedback> {
        let len = encoded.data.len();
        if len > PLAYER_MAX_FRAME {
            bail!(
                "frame {index} is {len} bytes even at quality {}, over the player's {PLAYER_MAX_FRAME}-byte limit; lower minquality or the size",
                encoded.quality
            );
        }
        self.bucket.add(len);
        self.stats.add(encoded, decided, self.quality);
        Ok(Feedback {
            estimate: decided.estimate,
            raw_estimate: decided.raw_estimate,
            actual: len,
            calibrate: encoded.quality == decided.quality,
        })
    }

    pub fn finish(mut self) -> Stats {
        self.stats.peak_fullness = self.bucket.peak;
        self.stats.buffer_overflows = self.bucket.overflows;
        self.stats
    }
}

#[derive(Clone, Debug)]
pub struct Stats {
    pub frames: usize,
    pub total_bytes: usize,
    pub min_bytes: usize,
    pub max_bytes: usize,
    pub lowered: usize,
    pub lowest_quality: u8,
    pub quality_sum: usize,
    pub over_max_frame: usize,
    pub requantized: usize,
    pub peak_fullness: f64,
    pub buffer_overflows: usize,
    pub estimate_error_sum: f64,
    pub estimate_error_max: f64,
}

impl Stats {
    fn new(quality: u8) -> Self {
        Self {
            frames: 0,
            total_bytes: 0,
            min_bytes: usize::MAX,
            max_bytes: 0,
            lowered: 0,
            lowest_quality: quality,
            quality_sum: 0,
            over_max_frame: 0,
            requantized: 0,
            peak_fullness: 0.0,
            buffer_overflows: 0,
            estimate_error_sum: 0.0,
            estimate_error_max: 0.0,
        }
    }

    pub fn report(&self, settings: &Settings, fps: f64) -> Vec<String> {
        let mut lines = Vec::new();
        if self.frames == 0 {
            return vec!["mjpeg: no frames".into()];
        }
        let frames = self.frames as f64;
        let kib = |bytes: usize| bytes as f64 / 1024.0;
        let seconds = frames / fps;
        lines.push(format!(
            "mjpeg: {} frames, {:.1} KiB average (min {:.1}, max {:.1}), {:.2} Mbit/s average",
            self.frames,
            kib(self.total_bytes / self.frames),
            kib(self.min_bytes),
            kib(self.max_bytes),
            self.total_bytes as f64 * 8.0 / seconds / 1e6,
        ));
        lines.push(format!(
            "mjpeg: quality {:.1} average, {} lowest, {} of {} frames below {}",
            self.quality_sum as f64 / frames,
            self.lowest_quality,
            self.lowered,
            self.frames,
            settings.quality,
        ));
        let estimated = self.frames - self.requantized;
        if estimated > 0 {
            lines.push(format!(
                "mjpeg: size estimate off by {:.1}% on average, {:.1}% at worst",
                self.estimate_error_sum / estimated as f64 * 100.0,
                self.estimate_error_max * 100.0,
            ));
        }
        lines.push(format!(
            "mjpeg: buffer peak {:.0}% of {} bytes at {} Mbit/s",
            self.peak_fullness / settings.buffer as f64 * 100.0,
            settings.buffer,
            settings.bitrate as f64 / 1e6,
        ));
        if self.buffer_overflows > 0 {
            lines.push(format!(
                "warning: {} frames went over the {} Mbit/s budget (buffer {} bytes)",
                self.buffer_overflows,
                settings.bitrate as f64 / 1e6,
                settings.buffer
            ));
        }
        if self.requantized > 0 {
            lines.push(format!(
                "mjpeg: {} frames re-quantised to stay within maxframe={}",
                self.requantized, settings.max_frame
            ));
        }
        if self.over_max_frame > 0 {
            lines.push(format!(
                "warning: {} frames exceed maxframe={} even at minquality={}",
                self.over_max_frame, settings.max_frame, settings.min_quality
            ));
        }
        lines
    }

    fn add(&mut self, encoded: &Encoded, decided: &Decision, quality: u8) {
        let len = encoded.data.len();
        self.frames += 1;
        self.total_bytes += len;
        self.min_bytes = self.min_bytes.min(len);
        self.max_bytes = self.max_bytes.max(len);
        if encoded.quality < quality {
            self.lowered += 1;
        }
        self.lowest_quality = self.lowest_quality.min(encoded.quality);
        self.quality_sum += encoded.quality as usize;
        if !encoded.fits {
            self.over_max_frame += 1;
        }
        if encoded.quality < decided.quality {
            self.requantized += 1;
        } else {
            let error = (len as f64 - decided.estimate as f64).abs() / len as f64;
            self.estimate_error_sum += error;
            self.estimate_error_max = self.estimate_error_max.max(error);
        }
    }
}
