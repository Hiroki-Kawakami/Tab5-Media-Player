// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::{BTreeMap, VecDeque};
use std::ffi::OsString;
use std::io::{ErrorKind, Read, Write};
use std::process::Stdio;
use std::sync::mpsc::{self, Receiver, Sender, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread;

use anyhow::{Context, Result, bail};

use tab5conv_core::jpeg::{self, Coefficients, Encoded, Frame, SizeModel};
use tab5conv_core::ratecontrol::{Bucket, RateControl};
use tab5conv_core::video::Picture;
use tab5conv_core::video::mjpeg::{PLAYER_MAX_FRAME, Settings};

use crate::process;

const LOOKAHEAD_SECONDS: f64 = 1.0;

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

    fn add(&mut self, done: &Done, quality: u8) {
        let len = done.encoded.data.len();
        self.frames += 1;
        self.total_bytes += len;
        self.min_bytes = self.min_bytes.min(len);
        self.max_bytes = self.max_bytes.max(len);
        if done.encoded.quality < quality {
            self.lowered += 1;
        }
        self.lowest_quality = self.lowest_quality.min(done.encoded.quality);
        self.quality_sum += done.encoded.quality as usize;
        if !done.encoded.fits {
            self.over_max_frame += 1;
        }
        if done.encoded.quality < done.decided_quality {
            self.requantized += 1;
        } else {
            let error = (len as f64 - done.estimate as f64).abs() / len as f64;
            self.estimate_error_sum += error;
            self.estimate_error_max = self.estimate_error_max.max(error);
        }
    }
}

struct Job {
    width: usize,
    height: usize,
    fps: f64,
    settings: Settings,
}

struct Analyzed {
    index: usize,
    coefficients: Coefficients,
    model: SizeModel,
}

struct Decided {
    index: usize,
    coefficients: Coefficients,
    quality: u8,
    estimate: f32,
    raw_estimate: f32,
}

struct Done {
    index: usize,
    encoded: Encoded,
    decided_quality: u8,
    estimate: f32,
    raw_estimate: f32,
}

struct Feedback {
    estimate: f32,
    raw_estimate: f32,
    actual: usize,
    calibrate: bool,
}

pub fn run(
    decode: &[OsString],
    mux: &[OsString],
    picture: &Picture,
    settings: &Settings,
) -> Result<Stats> {
    let job = &Job {
        width: picture.width as usize,
        height: picture.height as usize,
        fps: picture.rate.as_f64(),
        settings: *settings,
    };
    let mut decoder = process::spawn(decode, Stdio::null(), Stdio::piped())?;
    let mut muxer = match process::spawn(mux, Stdio::piped(), Stdio::inherit()) {
        Ok(child) => child,
        Err(err) => {
            let _ = decoder.kill();
            let _ = decoder.wait();
            return Err(err);
        }
    };
    let source = decoder.stdout.take().context("decoder has no stdout")?;
    let sink = muxer.stdin.take().context("muxer has no stdin")?;
    let result = pump(source, sink, job);
    let mut mux_failed_first = false;
    if result.is_err() {
        mux_failed_first = matches!(muxer.try_wait(), Ok(Some(status)) if !status.success());
        let _ = decoder.kill();
        let _ = muxer.kill();
    }
    let decoded = decoder.wait().context("waiting for ffmpeg (decode)")?;
    let muxed = muxer.wait().context("waiting for ffmpeg (mux)")?;
    if mux_failed_first {
        bail!("ffmpeg (mux) failed ({muxed})");
    }
    if !decoded.success() && result.is_ok() {
        bail!("ffmpeg (decode) failed ({decoded})");
    }
    let stats = result?;
    if !muxed.success() {
        bail!("ffmpeg (mux) failed ({muxed})");
    }
    Ok(stats)
}

fn read_frame(source: &mut impl Read, frame: &mut [u8]) -> Result<usize> {
    let mut filled = 0;
    while filled < frame.len() {
        match source.read(&mut frame[filled..]) {
            Ok(0) => break,
            Ok(n) => filled += n,
            Err(err) if err.kind() == ErrorKind::Interrupted => {}
            Err(err) => return Err(err).context("reading from ffmpeg (decode)"),
        }
    }
    Ok(filled)
}

type Queue<T> = Arc<Mutex<Receiver<T>>>;

fn next<T>(queue: &Queue<T>) -> Option<T> {
    queue.lock().ok()?.recv().ok()
}

fn pump(mut source: impl Read + Send, mut sink: impl Write, job: &Job) -> Result<Stats> {
    let frame_bytes = Frame::bytes(job.width, job.height);
    let workers = thread::available_parallelism().map_or(4, |n| n.get());
    let depth = workers * 2;
    let (raw_tx, raw_rx) = mpsc::sync_channel::<(usize, Vec<u8>)>(depth);
    let raw_rx: Queue<_> = Arc::new(Mutex::new(raw_rx));
    let (analyzed_tx, analyzed_rx) = mpsc::sync_channel::<Analyzed>(depth);
    let (decided_tx, decided_rx) = mpsc::sync_channel::<Decided>(depth);
    let decided_rx: Queue<_> = Arc::new(Mutex::new(decided_rx));
    let (done_tx, done_rx) = mpsc::sync_channel::<Done>(depth);
    let (feedback_tx, feedback_rx) = mpsc::channel::<Feedback>();

    thread::scope(|scope| {
        let reader = scope.spawn(move || -> Result<()> {
            let mut index = 0;
            loop {
                let mut frame = vec![0u8; frame_bytes];
                match read_frame(&mut source, &mut frame)? {
                    0 => return Ok(()),
                    n if n < frame_bytes => {
                        bail!("ffmpeg (decode) stopped in the middle of frame {index}")
                    }
                    _ => {}
                }
                if raw_tx.send((index, frame)).is_err() {
                    return Ok(());
                }
                index += 1;
            }
        });

        for _ in 0..workers {
            let raw_rx = Arc::clone(&raw_rx);
            let analyzed_tx = analyzed_tx.clone();
            scope.spawn(move || {
                while let Some((index, data)) = next(&raw_rx) {
                    let frame = Frame::from_yuv420p(&data, job.width, job.height);
                    let (coefficients, model) = jpeg::analyze(&frame);
                    let analyzed = Analyzed {
                        index,
                        coefficients,
                        model,
                    };
                    if analyzed_tx.send(analyzed).is_err() {
                        break;
                    }
                }
            });
        }
        drop(raw_rx);
        drop(analyzed_tx);

        scope.spawn(move || control(analyzed_rx, decided_tx, feedback_rx, job));

        let limits = job.settings.limits();
        for _ in 0..workers {
            let decided_rx = Arc::clone(&decided_rx);
            let done_tx = done_tx.clone();
            scope.spawn(move || {
                while let Some(decided) = next(&decided_rx) {
                    let encoded = jpeg::encode(&decided.coefficients, decided.quality, &limits);
                    let done = Done {
                        index: decided.index,
                        encoded,
                        decided_quality: decided.quality,
                        estimate: decided.estimate,
                        raw_estimate: decided.raw_estimate,
                    };
                    if done_tx.send(done).is_err() {
                        break;
                    }
                }
            });
        }
        drop(decided_rx);
        drop(done_tx);

        let written = write_in_order(done_rx, feedback_tx, &mut sink, job);
        let read = reader.join().expect("frame reader panicked");
        let stats = written?;
        read?;
        Ok(stats)
    })
}

fn control(
    analyzed: Receiver<Analyzed>,
    decided: SyncSender<Decided>,
    feedback: Receiver<Feedback>,
    job: &Job,
) {
    let fps = job.fps;
    let lookahead = ((fps * LOOKAHEAD_SECONDS).round() as usize).max(1);
    let mut rate = RateControl::new(&job.settings, fps);
    let mut pending = BTreeMap::new();
    let mut window: VecDeque<Analyzed> = VecDeque::new();
    let mut next_index = 0;
    let mut finished = false;
    while !finished || !window.is_empty() {
        match analyzed.recv() {
            Ok(frame) => {
                pending.insert(frame.index, frame);
                while let Some(frame) = pending.remove(&next_index) {
                    window.push_back(frame);
                    next_index += 1;
                }
            }
            Err(_) => finished = true,
        }
        for f in feedback.try_iter() {
            rate.feedback(f.estimate, f.raw_estimate, f.actual, f.calibrate);
        }
        while window.len() > lookahead || (finished && !window.is_empty()) {
            let models: Vec<&SizeModel> = window.iter().map(|f| &f.model).collect();
            let decision = rate.decide(&models);
            rate.commit(decision.estimate);
            let frame = window.pop_front().expect("window is not empty");
            let out = Decided {
                index: frame.index,
                coefficients: frame.coefficients,
                quality: decision.quality,
                estimate: decision.estimate,
                raw_estimate: decision.raw_estimate,
            };
            if decided.send(out).is_err() {
                return;
            }
        }
    }
}

fn write_in_order(
    done: Receiver<Done>,
    feedback: Sender<Feedback>,
    sink: &mut impl Write,
    job: &Job,
) -> Result<Stats> {
    let quality = job.settings.quality;
    let mut stats = Stats::new(quality);
    let mut bucket = Bucket::new(&job.settings, job.fps);
    let mut pending = BTreeMap::new();
    let mut next = 0;
    for frame in done {
        pending.insert(frame.index, frame);
        while let Some(frame) = pending.remove(&next) {
            let len = frame.encoded.data.len();
            if len > PLAYER_MAX_FRAME {
                bail!(
                    "frame {next} is {len} bytes even at quality {}, over the player's {PLAYER_MAX_FRAME}-byte limit; lower minquality or the size",
                    frame.encoded.quality
                );
            }
            sink.write_all(&frame.encoded.data)
                .context("writing to ffmpeg (mux)")?;
            let _ = feedback.send(Feedback {
                estimate: frame.estimate,
                raw_estimate: frame.raw_estimate,
                actual: len,
                calibrate: frame.encoded.quality == frame.decided_quality,
            });
            bucket.add(len);
            stats.add(&frame, quality);
            next += 1;
        }
    }
    sink.flush().context("writing to ffmpeg (mux)")?;
    stats.peak_fullness = bucket.peak;
    stats.buffer_overflows = bucket.overflows;
    Ok(stats)
}
