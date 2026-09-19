// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::io::{ErrorKind, Read, Write};
use std::process::Stdio;
use std::sync::mpsc::{self, Receiver};
use std::sync::{Arc, Mutex};
use std::thread;

use anyhow::{Context, Result, bail};

use crate::ffmpeg;
use crate::jpeg::{self, Encoded, Frame};
use crate::video::{MjpegJob, PLAYER_MAX_FRAME};

pub struct Stats {
    pub frames: usize,
    pub total_bytes: usize,
    pub min_bytes: usize,
    pub max_bytes: usize,
    pub lowered: usize,
    pub lowest_quality: u8,
    pub over_limit: usize,
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
            over_limit: 0,
        }
    }

    fn add(&mut self, encoded: &Encoded, quality: u8) {
        let len = encoded.data.len();
        self.frames += 1;
        self.total_bytes += len;
        self.min_bytes = self.min_bytes.min(len);
        self.max_bytes = self.max_bytes.max(len);
        if encoded.quality < quality {
            self.lowered += 1;
        }
        self.lowest_quality = self.lowest_quality.min(encoded.quality);
        if !encoded.fits {
            self.over_limit += 1;
        }
    }
}

pub fn run(decode: &[OsString], mux: &[OsString], job: &MjpegJob) -> Result<Stats> {
    let mut decoder = ffmpeg::spawn(decode, Stdio::null(), Stdio::piped())?;
    let mut muxer = match ffmpeg::spawn(mux, Stdio::piped(), Stdio::inherit()) {
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

fn pump(mut source: impl Read + Send, mut sink: impl Write, job: &MjpegJob) -> Result<Stats> {
    let frame_bytes = Frame::bytes(job.width, job.height);
    let workers = thread::available_parallelism().map_or(4, |n| n.get());
    let (work_tx, work_rx) = mpsc::sync_channel::<(usize, Vec<u8>)>(workers * 2);
    let work_rx = Arc::new(Mutex::new(work_rx));
    let (done_tx, done_rx) = mpsc::sync_channel::<(usize, Encoded)>(workers * 2);

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
                if work_tx.send((index, frame)).is_err() {
                    return Ok(());
                }
                index += 1;
            }
        });
        for _ in 0..workers {
            let work_rx = Arc::clone(&work_rx);
            let done_tx = done_tx.clone();
            scope.spawn(move || {
                loop {
                    let next = work_rx
                        .lock()
                        .map_err(|_| ())
                        .and_then(|rx| rx.recv().map_err(|_| ()));
                    let Ok((index, data)) = next else { break };
                    let frame = Frame::from_yuv420p(&data, job.width, job.height);
                    let encoded = jpeg::encode(&frame, &job.settings);
                    if done_tx.send((index, encoded)).is_err() {
                        break;
                    }
                }
            });
        }
        drop(work_rx);
        drop(done_tx);

        let written = write_in_order(done_rx, &mut sink, job);
        let read = reader.join().expect("frame reader panicked");
        let stats = written?;
        read?;
        Ok(stats)
    })
}

fn write_in_order(
    done: Receiver<(usize, Encoded)>,
    sink: &mut impl Write,
    job: &MjpegJob,
) -> Result<Stats> {
    let quality = job.settings.quality;
    let mut stats = Stats::new(quality);
    let mut pending = BTreeMap::new();
    let mut next = 0;
    for (index, encoded) in done {
        pending.insert(index, encoded);
        while let Some(encoded) = pending.remove(&next) {
            if encoded.data.len() > PLAYER_MAX_FRAME {
                bail!(
                    "frame {next} is {} bytes even at quality {}, over the player's {PLAYER_MAX_FRAME}-byte limit; lower minquality or the size",
                    encoded.data.len(),
                    encoded.quality
                );
            }
            sink.write_all(&encoded.data)
                .context("writing to ffmpeg (mux)")?;
            stats.add(&encoded, quality);
            next += 1;
        }
    }
    sink.flush().context("writing to ffmpeg (mux)")?;
    Ok(stats)
}
