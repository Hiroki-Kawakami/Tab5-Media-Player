// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod audio;
mod command;
mod ffmpeg;
mod framerate;
mod jpeg;
mod pipeline;
mod preset;
mod probe;
mod ratecontrol;
mod size;
mod spec;
mod video;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{Result, bail};
use clap::error::ErrorKind;
use clap::{CommandFactory, Parser};

#[derive(Parser)]
#[command(
    version,
    about = "Convert video files for Tab5-Media-Player",
    after_help = "Run with --preset help, --video help or --audio help for the details."
)]
struct Args {
    input: Option<PathBuf>,

    /// Output file (.mp4, .m4v, .mov or .mkv) [default: <input>.tab5.mp4]
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Settings for --video and --audio: tiny, small, default or quality
    #[arg(long, default_value = preset::DEFAULT, value_name = "NAME")]
    preset: String,

    /// Video codec and options, e.g. "h264,long=1280,short=720,crf=20"; adds to the preset's for the same codec
    #[arg(long, value_name = "SPEC")]
    video: Option<String>,

    /// Audio codec and options, e.g. "aac,bitrate=96k"; adds to the preset's for the same codec
    #[arg(long, value_name = "SPEC")]
    audio: Option<String>,

    /// Overwrite the output file if it exists
    #[arg(short = 'y', long)]
    overwrite: bool,

    /// Print the ffmpeg command instead of running it
    #[arg(long)]
    dry_run: bool,
}

fn default_output(input: &Path) -> PathBuf {
    let stem = input.file_stem().unwrap_or(input.as_os_str());
    let mut name = stem.to_os_string();
    name.push(".tab5.mp4");
    input.with_file_name(name)
}

fn run(args: Args) -> Result<()> {
    if args.preset == "help" {
        print!("{}", preset::help());
        return Ok(());
    }
    if args.video.as_deref() == Some("help") {
        print!("{}", video::HELP);
        return Ok(());
    }
    if args.audio.as_deref() == Some("help") {
        print!("{}", audio::HELP);
        return Ok(());
    }
    let preset = preset::find(&args.preset)?;
    let video_spec = preset.video(args.video.as_deref())?;
    let audio_spec = preset.audio(args.audio.as_deref())?;
    let applied = format!(
        "preset: {} (--video \"{}\" --audio \"{}\")",
        preset.name,
        video_spec.to_text(),
        audio_spec.to_text()
    );
    let video_spec = video::from_spec(video_spec)?;
    let audio_spec = audio::from_spec(audio_spec)?;

    let Some(input) = args.input.as_deref() else {
        Args::command()
            .error(
                ErrorKind::MissingRequiredArgument,
                "the input file is required",
            )
            .exit();
    };
    let output = args.output.clone().unwrap_or_else(|| default_output(input));
    let container = command::Container::from_path(&output)?;

    let info = probe::probe(input)?;
    let video = video_spec.plan(&info.video)?;
    let audio = audio_spec.plan(info.audio.as_ref())?;
    if !args.dry_run && !args.overwrite && output.exists() {
        bail!("{} already exists (use -y to overwrite)", output.display());
    }

    let encoders: Vec<&str> = video.encoder.into_iter().chain(audio.encoder).collect();
    ffmpeg::require_encoders(&encoders)?;

    eprintln!("{applied}");
    eprintln!(
        "video: {}x{} -> {}",
        info.video.display_width.round(),
        info.video.display_height.round(),
        video.label
    );
    eprintln!("audio: {}", audio.label);

    let job = command::Job {
        input,
        output: &output,
        container,
        overwrite: args.overwrite,
        video: &video,
        audio: &audio,
    };
    match job.commands() {
        command::Commands::Single(ffmpeg_args) => {
            if args.dry_run {
                println!("{}", ffmpeg::command_line(&ffmpeg_args));
                return Ok(());
            }
            ffmpeg::run(&ffmpeg_args)
        }
        command::Commands::Piped { decode, mux, job } => {
            if args.dry_run {
                println!("{} \\", ffmpeg::command_line(&decode));
                println!("  | (built-in MJPEG encoder) \\");
                println!("  | {}", ffmpeg::command_line(&mux));
                return Ok(());
            }
            let stats = pipeline::run(&decode, &mux, job)?;
            report(&stats, job);
            Ok(())
        }
    }
}

fn report(stats: &pipeline::Stats, job: &video::MjpegJob) {
    if stats.frames == 0 {
        eprintln!("mjpeg: no frames");
        return;
    }
    let settings = &job.settings;
    let frames = stats.frames as f64;
    let kib = |bytes: usize| bytes as f64 / 1024.0;
    let seconds = frames / job.rate.as_f64();
    eprintln!(
        "mjpeg: {} frames, {:.1} KiB average (min {:.1}, max {:.1}), {:.2} Mbit/s average",
        stats.frames,
        kib(stats.total_bytes / stats.frames),
        kib(stats.min_bytes),
        kib(stats.max_bytes),
        stats.total_bytes as f64 * 8.0 / seconds / 1e6,
    );
    eprintln!(
        "mjpeg: quality {:.1} average, {} lowest, {} of {} frames below {}",
        stats.quality_sum as f64 / frames,
        stats.lowest_quality,
        stats.lowered,
        stats.frames,
        settings.quality,
    );
    let estimated = stats.frames - stats.requantized;
    if estimated > 0 {
        eprintln!(
            "mjpeg: size estimate off by {:.1}% on average, {:.1}% at worst",
            stats.estimate_error_sum / estimated as f64 * 100.0,
            stats.estimate_error_max * 100.0,
        );
    }
    eprintln!(
        "mjpeg: buffer peak {:.0}% of {} bytes at {} Mbit/s",
        stats.peak_fullness / settings.buffer as f64 * 100.0,
        settings.buffer,
        settings.bitrate as f64 / 1e6,
    );
    if stats.buffer_overflows > 0 {
        eprintln!(
            "warning: {} frames went over the {} Mbit/s budget (buffer {} bytes)",
            stats.buffer_overflows,
            settings.bitrate as f64 / 1e6,
            settings.buffer
        );
    }
    if stats.requantized > 0 {
        eprintln!(
            "mjpeg: {} frames re-quantised to stay within maxframe={}",
            stats.requantized, settings.max_frame
        );
    }
    if stats.over_max_frame > 0 {
        eprintln!(
            "warning: {} frames exceed maxframe={} even at minquality={}",
            stats.over_max_frame, settings.max_frame, settings.min_quality
        );
    }
}

fn main() -> ExitCode {
    match run(Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("error: {err:#}");
            ExitCode::FAILURE
        }
    }
}
