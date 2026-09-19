// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::VecDeque;

pub struct Sample {
    pub time: i64,
    pub data: Vec<u8>,
    pub duration: u32,
    pub key: bool,
    pub offset: u32,
}

pub struct Interleaver {
    queues: Vec<VecDeque<Sample>>,
    open: Vec<bool>,
    queued: usize,
    max_queued: usize,
}

impl Interleaver {
    pub fn new(tracks: usize, max_queued_bytes: usize) -> Self {
        Self {
            queues: (0..tracks).map(|_| VecDeque::new()).collect(),
            open: vec![true; tracks],
            queued: 0,
            max_queued: max_queued_bytes,
        }
    }

    pub fn push(&mut self, track: usize, sample: Sample) {
        self.queued += sample.data.len();
        self.queues[track].push_back(sample);
    }

    pub fn close(&mut self, track: usize) {
        self.open[track] = false;
    }

    pub fn close_all(&mut self) {
        self.open.iter_mut().for_each(|open| *open = false);
    }

    pub fn pop(&mut self) -> Option<(usize, Sample)> {
        let (track, _) = self
            .queues
            .iter()
            .enumerate()
            .filter_map(|(i, q)| q.front().map(|s| (i, s.time)))
            .min_by_key(|&(_, time)| time)?;
        let waiting = self
            .queues
            .iter()
            .zip(&self.open)
            .any(|(q, &open)| open && q.is_empty());
        if waiting && self.queued <= self.max_queued {
            return None;
        }
        let sample = self.queues[track].pop_front()?;
        self.queued -= sample.data.len();
        Some((track, sample))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(time: i64) -> Sample {
        Sample {
            time,
            data: vec![0; 100],
            duration: 1,
            key: true,
            offset: 0,
        }
    }

    fn drain(i: &mut Interleaver) -> Vec<(usize, i64)> {
        std::iter::from_fn(|| i.pop().map(|(t, s)| (t, s.time))).collect()
    }

    #[test]
    fn waits_for_the_late_track() {
        let mut i = Interleaver::new(2, 1 << 20);
        for t in 0..5 {
            i.push(0, sample(t * 40_000));
        }
        assert!(drain(&mut i).is_empty());
        i.push(1, sample(0));
        i.push(1, sample(21_000));
        assert_eq!(drain(&mut i), [(0, 0), (1, 0), (1, 21_000)]);
        i.push(1, sample(42_000));
        i.push(1, sample(64_000));
        i.push(1, sample(85_000));
        assert_eq!(
            drain(&mut i),
            [
                (0, 40_000),
                (1, 42_000),
                (1, 64_000),
                (0, 80_000),
                (1, 85_000)
            ]
        );
        i.close_all();
        assert_eq!(drain(&mut i), [(0, 120_000), (0, 160_000)]);
    }

    #[test]
    fn stops_waiting_when_too_much_is_queued() {
        let mut i = Interleaver::new(2, 2500);
        for t in 0..30 {
            i.push(0, sample(t * 40_000));
        }
        let out = drain(&mut i);
        assert_eq!(out.first(), Some(&(0, 0)));
        assert_eq!(out.len(), 5);
        i.close(1);
        assert_eq!(drain(&mut i).len(), 25);
    }
}
