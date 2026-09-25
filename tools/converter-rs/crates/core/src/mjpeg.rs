// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::{BTreeMap, VecDeque};
use std::sync::Arc;

use anyhow::{Result, bail};

use crate::framerate::Rate;
use crate::jpeg::{Coefficients, Encoded, Similarity, SizeModel};
use crate::ratecontrol::{Bucket, Decision, RateControl};
use crate::video::mjpeg::{PLAYER_MAX_FRAME, Settings};

const LOOKAHEAD_SECONDS: f64 = 1.0;
const MAX_SPAN_SECONDS: f64 = 2.0;

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

/// A frame to store. `index` counts stored frames; `frame` is the input
/// frame it is, shown for `span` frame intervals.
#[derive(Clone, Copy, Debug)]
pub struct Decided {
    pub index: usize,
    pub frame: usize,
    pub span: usize,
    pub decision: Decision,
}

#[derive(Clone, Copy, Debug)]
pub struct Feedback {
    estimate: f32,
    raw_estimate: f32,
    actual: usize,
    calibrate: bool,
}

struct Analyzed {
    model: SizeModel,
    coefficients: Option<Arc<Coefficients>>,
}

struct Pending {
    frame: usize,
    model: SizeModel,
}

pub struct Scheduler {
    rate: RateControl,
    order: Reorder<Analyzed>,
    window: VecDeque<Pending>,
    similarity: Option<Similarity>,
    last: Option<(usize, Arc<Coefficients>)>,
    max_span: usize,
    end: usize,
    stored: usize,
    lookahead: usize,
}

impl Scheduler {
    pub fn new(settings: &Settings, fps: f64) -> Self {
        Self {
            rate: RateControl::new(settings, fps),
            order: Reorder::default(),
            window: VecDeque::new(),
            similarity: settings
                .dedup
                .then(|| Similarity::for_quality(settings.quality)),
            last: None,
            max_span: ((fps * MAX_SPAN_SECONDS).round() as usize).max(1),
            end: 0,
            stored: 0,
            lookahead: ((fps * LOOKAHEAD_SECONDS).round() as usize).max(1),
        }
    }

    /// Frames may arrive in any order. Returns the frames that were found to
    /// repeat the last stored one and will not be stored. Without
    /// coefficients a frame is always stored.
    pub fn push(
        &mut self,
        frame: usize,
        model: SizeModel,
        coefficients: Option<Arc<Coefficients>>,
    ) -> Vec<usize> {
        self.order.push(
            frame,
            Analyzed {
                model,
                coefficients,
            },
        );
        let mut repeated = Vec::new();
        while let Some(analyzed) = self.order.pop() {
            let frame = self.end;
            self.end += 1;
            if self.repeats(frame, analyzed.coefficients.as_deref()) {
                repeated.push(frame);
                continue;
            }
            if let (Some(_), Some(c)) = (&self.similarity, &analyzed.coefficients) {
                self.last = Some((frame, Arc::clone(c)));
            }
            self.window.push_back(Pending {
                frame,
                model: analyzed.model,
            });
        }
        repeated
    }

    fn repeats(&self, frame: usize, coefficients: Option<&Coefficients>) -> bool {
        match (&self.similarity, &self.last, coefficients) {
            (Some(similarity), Some((stored, last)), Some(c)) => {
                frame - stored < self.max_span && similarity.similar(last, c)
            }
            _ => false,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.window.is_empty()
    }

    pub fn decide(&mut self, finished: bool) -> Option<Decided> {
        let first = self.window.front()?.frame;
        let ready = finished || (self.window.len() > 1 && self.end - first > self.lookahead);
        if !ready {
            return None;
        }
        let spans: Vec<(&SizeModel, usize)> = self
            .window
            .iter()
            .enumerate()
            .map(|(i, p)| {
                let next = self.window.get(i + 1).map_or(self.end, |n| n.frame);
                (&p.model, next - p.frame)
            })
            .collect();
        let decision = self.rate.decide(&spans);
        let span = spans[0].1;
        self.rate.commit(decision.estimate, span);
        let pending = self.window.pop_front()?;
        let index = self.stored;
        self.stored += 1;
        Some(Decided {
            index,
            frame: pending.frame,
            span,
            decision,
        })
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
        frame: usize,
        span: usize,
        encoded: &Encoded,
        decided: &Decision,
    ) -> Result<Feedback> {
        let len = encoded.data.len();
        if len > PLAYER_MAX_FRAME {
            bail!(
                "frame {frame} is {len} bytes even at quality {}, over the player's {PLAYER_MAX_FRAME}-byte limit; lower minquality or the size",
                encoded.quality
            );
        }
        self.bucket.add(len, span);
        self.stats.add(encoded, decided, self.quality, span);
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
    pub input_frames: usize,
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
            input_frames: 0,
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
        let seconds = self.input_frames as f64 / fps;
        lines.push(format!(
            "mjpeg: {} frames, {:.1} KiB average (min {:.1}, max {:.1}), {:.2} Mbit/s average",
            self.frames,
            kib(self.total_bytes / self.frames),
            kib(self.min_bytes),
            kib(self.max_bytes),
            self.total_bytes as f64 * 8.0 / seconds / 1e6,
        ));
        if self.input_frames > self.frames {
            lines.push(format!(
                "mjpeg: {} of {} frames were unchanged and extend the frame before them",
                self.input_frames - self.frames,
                self.input_frames,
            ));
        }
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

    fn add(&mut self, encoded: &Encoded, decided: &Decision, quality: u8, span: usize) {
        let len = encoded.data.len();
        self.frames += 1;
        self.input_frames += span;
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::jpeg::{self, Frame, HuffmanMode};

    const W: usize = 64;
    const H: usize = 32;

    fn settings(dedup: bool) -> Settings {
        Settings {
            quality: 75,
            min_quality: 30,
            bitrate: 1_000_000_000,
            buffer: 1 << 20,
            max_frame: PLAYER_MAX_FRAME,
            huffman: HuffmanMode::Optimal,
            dedup,
        }
    }

    fn picture(seed: usize) -> (SizeModel, Arc<Coefficients>) {
        let data: Vec<u8> = (0..Frame::bytes(W, H))
            .map(|i| ((i * 7 + seed * 131) ^ (i / W * seed)) as u8)
            .collect();
        let (coefficients, model) = jpeg::analyze(&Frame::from_yuv420p(&data, W, H));
        (model, Arc::new(coefficients))
    }

    fn run(
        dedup: bool,
        fps: f64,
        pictures: &[usize],
        order: &[usize],
    ) -> (Vec<Decided>, Vec<usize>) {
        let mut scheduler = Scheduler::new(&settings(dedup), fps);
        let mut decided = Vec::new();
        let mut repeated = Vec::new();
        for &frame in order {
            let (model, coefficients) = picture(pictures[frame]);
            repeated.extend(scheduler.push(frame, model, Some(coefficients)));
            decided.extend(std::iter::from_fn(|| scheduler.decide(false)));
        }
        decided.extend(std::iter::from_fn(|| scheduler.decide(true)));
        (decided, repeated)
    }

    fn placed(decided: &[Decided]) -> Vec<(usize, usize, usize)> {
        decided.iter().map(|d| (d.index, d.frame, d.span)).collect()
    }

    #[test]
    fn repeated_frames_extend_the_stored_one() {
        let pictures = [1, 1, 1, 2, 3, 3, 1];
        let (decided, repeated) = run(true, 30.0, &pictures, &[1, 0, 3, 2, 5, 4, 6]);
        assert_eq!(repeated, [1, 2, 5]);
        assert_eq!(
            placed(&decided),
            [(0, 0, 3), (1, 3, 1), (2, 4, 2), (3, 6, 1)]
        );
    }

    #[test]
    fn every_frame_is_stored_without_dedup() {
        let pictures = [1, 1, 1, 2];
        let (decided, repeated) = run(false, 30.0, &pictures, &[0, 1, 2, 3]);
        assert!(repeated.is_empty());
        assert_eq!(
            placed(&decided),
            [(0, 0, 1), (1, 1, 1), (2, 2, 1), (3, 3, 1)]
        );
    }

    #[test]
    fn a_frame_is_stored_at_least_every_two_seconds() {
        let pictures = [1; 12];
        let order: Vec<usize> = (0..12).collect();
        let (decided, repeated) = run(true, 2.5, &pictures, &order);
        assert_eq!(repeated.len(), 12 - 3);
        assert_eq!(placed(&decided), [(0, 0, 5), (1, 5, 5), (2, 10, 2)]);
    }

    #[test]
    fn decisions_wait_for_the_next_stored_frame() {
        let mut scheduler = Scheduler::new(&settings(true), 2.0);
        let (model, c) = picture(1);
        scheduler.push(0, model, Some(c));
        for frame in 1..4 {
            let (model, c) = picture(1);
            scheduler.push(frame, model, Some(c));
            assert!(scheduler.decide(false).is_none());
        }
        let (model, c) = picture(2);
        scheduler.push(4, model, Some(c));
        let d = scheduler.decide(false).unwrap();
        assert_eq!((d.frame, d.span), (0, 4));
    }
}
