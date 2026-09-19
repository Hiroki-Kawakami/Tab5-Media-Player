// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod audio;
mod command;
mod ffmpeg;
mod framerate;
mod jpeg;
mod pipeline;
mod probe;
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
    after_help = "Run with --video help or --audio help for the codecs and their keys."
)]
struct Args {
    input: Option<PathBuf>,

    /// Output file (.mp4, .m4v, .mov or .mkv) [default: <input>.tab5.mp4]
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Video codec and options, e.g. "h264,long=1280,short=720,crf=20"
    #[arg(long, default_value = "h264", value_name = "SPEC")]
    video: String,

    /// Audio codec and options, e.g. "aac,bitrate=96k"
    #[arg(long, default_value = "auto", value_name = "SPEC")]
    audio: String,

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
    if args.video == "help" || args.audio == "help" {
        print!(
            "{}",
            if args.video == "help" {
                video::HELP
            } else {
                audio::HELP
            }
        );
        return Ok(());
    }
    let video_spec = video::parse(&args.video)?;
    let audio_spec = audio::parse(&args.audio)?;

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
    let kib = |bytes: usize| bytes as f64 / 1024.0;
    eprintln!(
        "mjpeg: {} frames, {:.1} KiB average (min {:.1}, max {:.1})",
        stats.frames,
        kib(stats.total_bytes / stats.frames),
        kib(stats.min_bytes),
        kib(stats.max_bytes),
    );
    if stats.lowered > 0 {
        eprintln!(
            "mjpeg: {} frames lowered from quality {} to fit (lowest {})",
            stats.lowered, job.settings.quality, stats.lowest_quality
        );
    }
    if stats.over_limit > 0 {
        eprintln!(
            "warning: {} frames exceed maxframe={} even at minquality={}",
            stats.over_limit, job.settings.max_frame, job.settings.min_quality
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
