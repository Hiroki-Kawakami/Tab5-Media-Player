// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{Context, Result, bail};
use clap::error::ErrorKind;
use clap::{CommandFactory, Parser};
use tab5conv_core::container::Container;
use tab5conv_core::video::VideoCodec;
use tab5conv_core::video::mjpeg::Settings as MjpegSettings;
use tab5conv_core::{Specs, audio, preset, video};
use tab5conv_ffmpeg::{Conversion, Stats, batch};

#[derive(Parser)]
#[command(
    version,
    about = "Convert video files for Tab5-Media-Player",
    after_help = "Run with --preset help, --video help or --audio help for the details."
)]
struct Args {
    /// Input files; with several, names containing ".tab5." are skipped
    #[arg(value_name = "INPUT")]
    inputs: Vec<PathBuf>,

    /// Output file for a single input (.mp4, .m4v, .mov or .mkv) [default: <input>.tab5.mp4]
    #[arg(short, long, conflicts_with = "outdir")]
    output: Option<PathBuf>,

    /// Directory for the outputs, named <input>.mp4; created if missing
    #[arg(long, value_name = "DIR")]
    outdir: Option<PathBuf>,

    /// Settings for --video and --audio: tiny, small, default or quality
    #[arg(long, default_value = preset::DEFAULT, value_name = "NAME")]
    preset: String,

    /// Video codec and options, e.g. "h264,long=1280,short=720,crf=20"; adds to the preset's for the same codec
    #[arg(long, value_name = "SPEC")]
    video: Option<String>,

    /// Audio codec and options, e.g. "aac,bitrate=96k"; adds to the preset's for the same codec
    #[arg(long, value_name = "SPEC")]
    audio: Option<String>,

    /// Overwrite output files that already exist
    #[arg(short = 'y', long)]
    overwrite: bool,

    /// Print the ffmpeg command instead of running it
    #[arg(long)]
    dry_run: bool,
}

fn convert(
    input: &Path,
    output: &Path,
    container: Container,
    specs: &Specs,
    dry_run: bool,
) -> Result<()> {
    let conversion = Conversion::prepare(input, output, container, specs)?;
    let source = &conversion.info.video;
    eprintln!(
        "video: {}x{} -> {}",
        source.display_width.round(),
        source.display_height.round(),
        conversion.video.label
    );
    eprintln!("audio: {}", conversion.audio.label);
    eprintln!("output: {}", output.display());

    if dry_run {
        for line in conversion.command_lines() {
            println!("{line}");
        }
        return Ok(());
    }
    if let (Some(stats), VideoCodec::Mjpeg(settings)) = (conversion.run()?, &conversion.video.codec)
    {
        report(&stats, settings, conversion.video.picture.rate.as_f64());
    }
    Ok(())
}

fn run(args: Args) -> Result<bool> {
    if args.preset == "help" {
        print!("{}", preset::help());
        return Ok(true);
    }
    if args.video.as_deref() == Some("help") {
        print!("{}", video::HELP);
        return Ok(true);
    }
    if args.audio.as_deref() == Some("help") {
        print!("{}", audio::HELP);
        return Ok(true);
    }
    let specs = Specs::resolve(&args.preset, args.video.as_deref(), args.audio.as_deref())?;
    let applied = format!(
        "preset: {} (--video \"{}\" --audio \"{}\")",
        specs.preset, specs.video_text, specs.audio_text
    );

    if args.inputs.is_empty() {
        Args::command()
            .error(
                ErrorKind::MissingRequiredArgument,
                "at least one input file is required",
            )
            .exit();
    }
    let targets = batch::plan(&args.inputs, args.output.as_deref(), args.outdir.as_deref())?;
    if !args.dry_run && !args.overwrite {
        let existing = batch::existing(&targets);
        if !existing.is_empty() {
            let list: Vec<String> = existing.iter().map(|p| p.display().to_string()).collect();
            bail!(
                "these outputs already exist (use -y to overwrite):\n       {}",
                list.join("\n       ")
            );
        }
    }
    if let (Some(dir), false) = (&args.outdir, args.dry_run) {
        std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
    }

    eprintln!("{applied}");
    if let [
        batch::Target::Convert {
            input,
            output,
            container,
        },
    ] = targets.as_slice()
    {
        convert(input, output, *container, &specs, args.dry_run)?;
        return Ok(true);
    }

    let total = targets.len();
    let (mut converted, mut skipped, mut failed) = (0, Vec::new(), Vec::new());
    for (i, target) in targets.iter().enumerate() {
        match target {
            batch::Target::Skip { input, reason } => {
                eprintln!(
                    "[{}/{total}] {}: skipped ({reason})",
                    i + 1,
                    input.display()
                );
                skipped.push(format!("{} ({reason})", input.display()));
            }
            batch::Target::Convert {
                input,
                output,
                container,
            } => {
                eprintln!("[{}/{total}] {}", i + 1, input.display());
                match convert(input, output, *container, &specs, args.dry_run) {
                    Ok(()) => converted += 1,
                    Err(err) => {
                        eprintln!("error: {err:#}");
                        failed.push(format!("{}: {err:#}", input.display()));
                    }
                }
            }
        }
    }
    eprintln!(
        "done: {converted} {}, {} skipped, {} failed",
        if args.dry_run {
            "planned (dry run)"
        } else {
            "converted"
        },
        skipped.len(),
        failed.len()
    );
    for line in &skipped {
        eprintln!("  skipped  {line}");
    }
    for line in &failed {
        eprintln!("  failed   {line}");
    }
    Ok(failed.is_empty())
}

fn report(stats: &Stats, settings: &MjpegSettings, fps: f64) {
    if stats.frames == 0 {
        eprintln!("mjpeg: no frames");
        return;
    }
    let frames = stats.frames as f64;
    let kib = |bytes: usize| bytes as f64 / 1024.0;
    let seconds = frames / fps;
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
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(err) => {
            eprintln!("error: {err:#}");
            ExitCode::FAILURE
        }
    }
}
