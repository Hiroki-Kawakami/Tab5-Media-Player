// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::{HashMap, VecDeque};
use std::ffi::OsString;
use std::fs::File;
use std::io::{ErrorKind, Read, Write};
use std::path::Path;
use std::process::Stdio;
use std::sync::atomic::Ordering;
use std::sync::mpsc::{self, Receiver, Sender, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};

use tab5conv_core::audio::{AudioAction, AudioPlan};
use tab5conv_core::container::demux::{self, Codec};
use tab5conv_core::container::interleave::{Interleaver, Sample};
use tab5conv_core::container::io::Stream;
use tab5conv_core::container::mux::{Mp4Muxer, MuxCodec, MuxKind, MuxTrack};
use tab5conv_core::framerate::Rate;
use tab5conv_core::jpeg::{self, Coefficients, Encoded, Frame, SizeModel};
use tab5conv_core::mjpeg::{self, Feedback, Reorder, Scheduler, Stats, Tally};
use tab5conv_core::ratecontrol::Decision;
use tab5conv_core::video::Picture;
use tab5conv_core::video::mjpeg::Settings;

use crate::process::{self, Cancelled, Log, Monitor, Tools};

const VIDEO_TRACK: usize = 0;
const AUDIO_TRACK: usize = 1;
const MAX_INTERLEAVE_BYTES: usize = 16 << 20;
const AAC_FRAME: u32 = 1024;
const MP3_FRAME: u32 = 1152;
const STATS_INTERVAL: Duration = Duration::from_millis(100);

pub struct Job<'a> {
    pub tools: &'a Tools,
    pub decode: &'a [OsString],
    pub mux: Option<&'a [OsString]>,
    pub input: &'a Path,
    pub output: &'a Path,
    pub picture: &'a Picture,
    pub audio: &'a AudioPlan,
    pub settings: &'a Settings,
}

struct Encode {
    width: usize,
    height: usize,
    fps: f64,
    rate: Rate,
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

impl Job<'_> {
    fn encode(&self) -> Encode {
        Encode {
            width: self.picture.width as usize,
            height: self.picture.height as usize,
            fps: self.picture.rate.as_f64(),
            rate: self.picture.rate,
            settings: *self.settings,
        }
    }
}

pub fn run(job: &Job, monitor: Option<&Monitor>) -> Result<Stats> {
    match job.mux {
        Some(mux) => with_ffmpeg_mux(job, mux, monitor),
        None => self_muxed(job, monitor),
    }
}

fn stderr_for(monitor: Option<&Monitor>) -> Stdio {
    if monitor.is_some() {
        Stdio::piped()
    } else {
        Stdio::inherit()
    }
}

fn self_muxed(job: &Job, monitor: Option<&Monitor>) -> Result<Stats> {
    let encode = job.encode();
    let mut child = job.tools.spawn(
        job.decode,
        Stdio::null(),
        Stdio::piped(),
        stderr_for(monitor),
    )?;
    let log = child.stderr.take().map(Log::capture);
    let stdout = child.stdout.take().context("ffmpeg has no stdout")?;
    let result = mux_stream(job, &encode, stdout, monitor);
    if result.is_err() {
        let _ = child.kill();
    }
    let status = child.wait().context("waiting for ffmpeg")?;
    let log = log.map(Log::finish).unwrap_or_default();
    if !status.success() && !matches!(&result, Err(err) if err.is::<Cancelled>()) {
        bail!("{}", process::failure("ffmpeg", status, &log));
    }
    result
}

struct Audio {
    track: u32,
    rate: u32,
    frame: u32,
}

impl Audio {
    fn samples(&self, duration_us: i64) -> u32 {
        match duration_us {
            0 => self.frame,
            us => ((us as i128 * self.rate as i128 + 500_000) / 1_000_000) as u32,
        }
    }
}

fn mux_stream(
    job: &Job,
    encode: &Encode,
    stdout: impl Read + Send,
    monitor: Option<&Monitor>,
) -> Result<Stats> {
    let mut demuxer = demux::open(Stream::new(stdout))?;
    let info = demuxer.info();
    let (video, _) = info.video().context("ffmpeg produced no video track")?;
    let video_track = video.index;
    let picture = job.picture;
    let mut tracks = vec![MuxTrack {
        codec: MuxCodec::Mjpeg,
        kind: MuxKind::Video {
            width: picture.width,
            height: picture.height,
            display_rotation: picture.rotation.and_then(|r| r.display_rotation),
        },
        timescale: mjpeg::TIMESCALE,
    }];
    let mut skip = 0;
    let audio = match info.audio() {
        Some((track, audio)) => {
            let (codec, frame) = match track.codec {
                Codec::Aac => (
                    MuxCodec::Aac {
                        config: track.extradata.clone(),
                    },
                    AAC_FRAME,
                ),
                Codec::Mp3 => (MuxCodec::Mp3, MP3_FRAME),
                other => bail!("mp4 cannot hold {} audio", other.name()),
            };
            tracks.push(MuxTrack {
                codec,
                kind: MuxKind::Audio {
                    channels: audio.channels,
                    sample_rate: audio.sample_rate,
                },
                timescale: audio.sample_rate,
            });
            skip = audio.priming.max(source_priming(job));
            Some(Audio {
                track: track.index,
                rate: audio.sample_rate,
                frame,
            })
        }
        None => None,
    };
    let selected: Vec<u32> = [Some(video_track), audio.as_ref().map(|a| a.track)]
        .into_iter()
        .flatten()
        .collect();
    demuxer.select(&selected);

    let file =
        File::create(job.output).with_context(|| format!("creating {}", job.output.display()))?;
    let mut muxer = Mp4Muxer::new(file, tracks)?;
    if skip > 0 {
        muxer.set_skip(AUDIO_TRACK, skip)?;
    }

    let queue = &AudioQueue::default();
    let frame_bytes = Frame::bytes(encode.width, encode.height);
    let has_audio = audio.is_some();
    let reader = move |raw: SyncSender<(usize, Vec<u8>)>| -> Result<()> {
        let mut index = 0;
        while let Some(packet) = demuxer.next_packet()? {
            if packet.track == video_track {
                if packet.data.len() != frame_bytes {
                    bail!(
                        "ffmpeg produced a {}-byte frame, expected {frame_bytes}",
                        packet.data.len()
                    );
                }
                if raw.send((index, packet.data)).is_err() {
                    break;
                }
                index += 1;
            } else if let Some(audio) = &audio
                && packet.track == audio.track
            {
                queue.push(Sample {
                    time: packet.pts,
                    duration: audio.samples(packet.duration),
                    data: packet.data,
                    key: true,
                    offset: 0,
                });
            }
        }
        queue.close();
        Ok(())
    };

    let mut output = Output {
        muxer,
        interleaver: Interleaver::new(if has_audio { 2 } else { 1 }, MAX_INTERLEAVE_BYTES),
        queue: has_audio.then_some(queue),
        rate: encode.rate,
        frames: 0,
    };
    let stats = pump(
        reader,
        &mut |encoded| output.frame(encoded),
        encode,
        monitor,
        monitor.is_none(),
    )?;
    output.finish()?;
    Ok(stats)
}

fn source_priming(job: &Job) -> u32 {
    let AudioAction::Copy { index } = job.audio.action else {
        return 0;
    };
    let Ok(source) = File::open(job.input) else {
        return 0;
    };
    let Ok(demuxer) = demux::open(source) else {
        return 0;
    };
    let tracks = &demuxer.info().tracks;
    tracks
        .iter()
        .find(|t| t.index == index)
        .or_else(|| tracks.iter().find(|t| t.audio().is_some()))
        .and_then(|t| t.audio())
        .map_or(0, |a| a.priming)
}

#[derive(Default)]
struct AudioQueue {
    state: Mutex<(VecDeque<Sample>, bool)>,
}

impl AudioQueue {
    fn push(&self, sample: Sample) {
        self.state.lock().expect("audio queue").0.push_back(sample);
    }

    fn close(&self) {
        self.state.lock().expect("audio queue").1 = true;
    }

    fn drain(&self, interleaver: &mut Interleaver) {
        let mut state = self.state.lock().expect("audio queue");
        for sample in state.0.drain(..) {
            interleaver.push(AUDIO_TRACK, sample);
        }
        if state.1 {
            interleaver.close(AUDIO_TRACK);
        }
    }
}

struct Output<'a> {
    muxer: Mp4Muxer<File>,
    interleaver: Interleaver,
    queue: Option<&'a AudioQueue>,
    rate: Rate,
    frames: u64,
}

impl Output<'_> {
    fn frame(&mut self, encoded: Encoded) -> Result<()> {
        let (start, end) = (
            mjpeg::ticks(self.rate, self.frames),
            mjpeg::ticks(self.rate, self.frames + 1),
        );
        self.interleaver.push(
            VIDEO_TRACK,
            Sample {
                time: (start as i128 * 1_000_000 / mjpeg::TIMESCALE as i128) as i64,
                data: encoded.data,
                duration: (end - start) as u32,
                key: true,
                offset: 0,
            },
        );
        self.frames += 1;
        self.drain_audio();
        self.write_ready()
    }

    fn drain_audio(&mut self) {
        if let Some(queue) = self.queue {
            queue.drain(&mut self.interleaver);
        }
    }

    fn write_ready(&mut self) -> Result<()> {
        while let Some((track, sample)) = self.interleaver.pop() {
            self.muxer.write_with_offset(
                track,
                &sample.data,
                sample.duration,
                sample.key,
                sample.offset,
            )?;
        }
        Ok(())
    }

    fn finish(mut self) -> Result<()> {
        self.drain_audio();
        self.interleaver.close_all();
        self.write_ready()?;
        self.muxer.finish()?;
        Ok(())
    }
}

fn with_ffmpeg_mux(job: &Job, mux: &[OsString], monitor: Option<&Monitor>) -> Result<Stats> {
    let encode = job.encode();
    let tools = job.tools;
    let mut decoder = tools.spawn(
        job.decode,
        Stdio::null(),
        Stdio::piped(),
        stderr_for(monitor),
    )?;
    let mut muxer = match tools.spawn(mux, Stdio::piped(), Stdio::inherit(), stderr_for(monitor)) {
        Ok(child) => child,
        Err(err) => {
            let _ = decoder.kill();
            let _ = decoder.wait();
            return Err(err);
        }
    };
    let decode_log = decoder.stderr.take().map(Log::capture);
    let mux_log = muxer.stderr.take().map(Log::capture);
    let mut source = decoder.stdout.take().context("decoder has no stdout")?;
    let mut sink = muxer.stdin.take().context("muxer has no stdin")?;
    let frame_bytes = Frame::bytes(encode.width, encode.height);
    let reader = move |raw: SyncSender<(usize, Vec<u8>)>| -> Result<()> {
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
            if raw.send((index, frame)).is_err() {
                return Ok(());
            }
            index += 1;
        }
    };
    let result = pump(
        reader,
        &mut |encoded: Encoded| {
            sink.write_all(&encoded.data)
                .context("writing to ffmpeg (mux)")
        },
        &encode,
        monitor,
        false,
    );
    let result = result.and_then(|stats| {
        sink.flush().context("writing to ffmpeg (mux)")?;
        Ok(stats)
    });
    drop(sink);
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
    reader: impl FnOnce(SyncSender<(usize, Vec<u8>)>) -> Result<()> + Send,
    write: &mut dyn FnMut(Encoded) -> Result<()>,
    job: &Encode,
    monitor: Option<&Monitor>,
    stats: bool,
) -> Result<Stats> {
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
        let reader = scope.spawn(move || reader(raw_tx));

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

        let written = write_in_order(done_rx, feedback_tx, write, job, monitor, stats);
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
    job: &Encode,
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
    write: &mut dyn FnMut(Encoded) -> Result<()>,
    job: &Encode,
    monitor: Option<&Monitor>,
    stats: bool,
) -> Result<Stats> {
    let mut tally = Tally::new(&job.settings, job.fps);
    let mut order = Reorder::default();
    let mut progress = Progress::new(job.fps, stats);
    let mut written = 0;
    for frame in done {
        order.push(frame.index, frame);
        while let Some(frame) = order.pop() {
            let f = tally.record(frame.index, &frame.encoded, &frame.decision)?;
            let quality = frame.decision.quality;
            let bytes = frame.encoded.data.len();
            write(frame.encoded)?;
            let _ = feedback.send(f);
            written += 1;
            progress.frame(written, bytes, quality);
            if let Some(monitor) = monitor {
                if monitor.cancel.load(Ordering::Relaxed) {
                    return Err(Cancelled.into());
                }
                (monitor.progress)(written as f64 / job.fps);
            }
        }
    }
    progress.finish(written);
    Ok(tally.finish())
}

struct Progress {
    enabled: bool,
    fps: f64,
    started: Instant,
    shown: Instant,
    bytes: u64,
    quality: u8,
}

impl Progress {
    fn new(fps: f64, enabled: bool) -> Self {
        let now = Instant::now();
        Self {
            enabled,
            fps,
            started: now,
            shown: now,
            bytes: 0,
            quality: 0,
        }
    }

    fn frame(&mut self, written: u64, bytes: usize, quality: u8) {
        self.bytes += bytes as u64;
        self.quality = quality;
        if !self.enabled {
            return;
        }
        let now = Instant::now();
        if now.duration_since(self.shown) < STATS_INTERVAL {
            return;
        }
        self.shown = now;
        self.show(written, quality);
    }

    fn show(&self, written: u64, quality: u8) {
        let elapsed = self.started.elapsed().as_secs_f64().max(1e-9);
        let time = written as f64 / self.fps;
        let (minutes, seconds) = ((time as u64) / 60, time % 60.0);
        eprint!(
            "\rframe={written:5} fps={:5.1} q={quality:3} size={:7}KiB time={:02}:{:02}:{seconds:05.2} speed={:5.2}x",
            written as f64 / elapsed,
            self.bytes / 1024,
            minutes / 60,
            minutes % 60,
            time / elapsed,
        );
    }

    fn finish(&self, written: u64) {
        if self.enabled && written > 0 {
            self.show(written, self.quality);
            eprintln!();
        }
    }
}
