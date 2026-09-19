// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::HashMap;
use std::ffi::OsString;
use std::io::{ErrorKind, Read, Write};
use std::process::Stdio;
use std::sync::atomic::Ordering;
use std::sync::mpsc::{self, Receiver, Sender, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread;

use anyhow::{Context, Result, bail};

use tab5conv_core::jpeg::{self, Coefficients, Encoded, Frame, SizeModel};
use tab5conv_core::mjpeg::{Feedback, Reorder, Scheduler, Stats, Tally};
use tab5conv_core::ratecontrol::Decision;
use tab5conv_core::video::Picture;
use tab5conv_core::video::mjpeg::Settings;

use crate::process::{self, Cancelled, Log, Monitor, Tools};

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
    decision: Decision,
}

struct Done {
    index: usize,
    encoded: Encoded,
    decision: Decision,
}

pub fn run(
    tools: &Tools,
    decode: &[OsString],
    mux: &[OsString],
    picture: &Picture,
    settings: &Settings,
    monitor: Option<&Monitor>,
) -> Result<Stats> {
    let job = &Job {
        width: picture.width as usize,
        height: picture.height as usize,
        fps: picture.rate.as_f64(),
        settings: *settings,
    };
    let stderr = || {
        if monitor.is_some() {
            Stdio::piped()
        } else {
            Stdio::inherit()
        }
    };
    let mut decoder = tools.spawn(decode, Stdio::null(), Stdio::piped(), stderr())?;
    let mut muxer = match tools.spawn(mux, Stdio::piped(), Stdio::inherit(), stderr()) {
        Ok(child) => child,
        Err(err) => {
            let _ = decoder.kill();
            let _ = decoder.wait();
            return Err(err);
        }
    };
    let decode_log = decoder.stderr.take().map(Log::capture);
    let mux_log = muxer.stderr.take().map(Log::capture);
    let source = decoder.stdout.take().context("decoder has no stdout")?;
    let sink = muxer.stdin.take().context("muxer has no stdin")?;
    let result = pump(source, sink, job, monitor);
    let cancelled = matches!(&result, Err(err) if err.is::<Cancelled>());
    let mut mux_failed_first = false;
    if result.is_err() {
        mux_failed_first =
            !cancelled && matches!(muxer.try_wait(), Ok(Some(status)) if !status.success());
        let _ = decoder.kill();
        let _ = muxer.kill();
    }
    let decoded = decoder.wait().context("waiting for ffmpeg (decode)")?;
    let muxed = muxer.wait().context("waiting for ffmpeg (mux)")?;
    let decode_log = decode_log.map(Log::finish).unwrap_or_default();
    let mux_log = mux_log.map(Log::finish).unwrap_or_default();
    if mux_failed_first {
        bail!("{}", process::failure("ffmpeg (mux)", muxed, &mux_log));
    }
    if !decoded.success() && result.is_ok() {
        bail!(
            "{}",
            process::failure("ffmpeg (decode)", decoded, &decode_log)
        );
    }
    let stats = result?;
    if !muxed.success() {
        bail!("{}", process::failure("ffmpeg (mux)", muxed, &mux_log));
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

fn pump(
    mut source: impl Read + Send,
    mut sink: impl Write,
    job: &Job,
    monitor: Option<&Monitor>,
) -> Result<Stats> {
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
                    let encoded =
                        jpeg::encode(&decided.coefficients, decided.decision.quality, &limits);
                    let done = Done {
                        index: decided.index,
                        encoded,
                        decision: decided.decision,
                    };
                    if done_tx.send(done).is_err() {
                        break;
                    }
                }
            });
        }
        drop(decided_rx);
        drop(done_tx);

        let written = write_in_order(done_rx, feedback_tx, &mut sink, job, monitor);
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
    let mut scheduler = Scheduler::new(&job.settings, job.fps);
    let mut coefficients = HashMap::new();
    let mut finished = false;
    while !finished || !scheduler.is_empty() {
        match analyzed.recv() {
            Ok(frame) => {
                coefficients.insert(frame.index, frame.coefficients);
                scheduler.push(frame.index, frame.model);
            }
            Err(_) => finished = true,
        }
        for f in feedback.try_iter() {
            scheduler.feedback(&f);
        }
        while let Some(d) = scheduler.decide(finished) {
            let out = Decided {
                index: d.index,
                coefficients: coefficients.remove(&d.index).expect("analyzed frame"),
                decision: d.decision,
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
    monitor: Option<&Monitor>,
) -> Result<Stats> {
    let mut tally = Tally::new(&job.settings, job.fps);
    let mut order = Reorder::default();
    let mut written = 0;
    for frame in done {
        order.push(frame.index, frame);
        while let Some(frame) = order.pop() {
            let f = tally.record(frame.index, &frame.encoded, &frame.decision)?;
            sink.write_all(&frame.encoded.data)
                .context("writing to ffmpeg (mux)")?;
            let _ = feedback.send(f);
            written += 1;
            if let Some(monitor) = monitor {
                if monitor.cancel.load(Ordering::Relaxed) {
                    return Err(Cancelled.into());
                }
                (monitor.progress)(written as f64 / job.fps);
            }
        }
    }
    sink.flush().context("writing to ffmpeg (mux)")?;
    Ok(tally.finish())
}
