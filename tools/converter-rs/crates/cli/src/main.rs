// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{Context, Result, bail};
use clap::error::ErrorKind;
use clap::{CommandFactory, Parser};
use tab5conv_core::container::Container;
use tab5conv_core::video::VideoCodec;
use tab5conv_core::{Specs, audio, preset, thumbnail, video};
use tab5conv_ffmpeg::{Conversion, Tools, batch};

#[derive(Parser)]
#[command(
    version,
    about = "Convert video files for Tab5-Media-Player",
    after_help = "Run with --preset help, --video help, --audio help or --thumbnail help for the details."
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

    /// Cover art options, e.g. "jpeg,at=5,long=480", or "none" [default: jpeg]
    #[arg(long, value_name = "SPEC")]
    thumbnail: Option<String>,

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
    let conversion = Conversion::prepare(&Tools::default(), input, output, container, specs)?;
    let source = &conversion.info.video;
    eprintln!(
        "video: {}x{} -> {}",
        source.display_width.round(),
        source.display_height.round(),
        conversion.video.label
    );
    eprintln!("audio: {}", conversion.audio.label);
    eprintln!("thumbnail: {}", conversion.thumbnail_label);
    eprintln!("output: {}", output.display());

    if dry_run {
        for line in conversion.command_lines() {
            println!("{line}");
        }
        return Ok(());
    }
    if let (Some(stats), VideoCodec::Mjpeg(settings)) =
        (conversion.run(None)?, &conversion.video.codec)
    {
        for line in stats.report(settings, conversion.video.picture.rate.as_f64()) {
            eprintln!("{line}");
        }
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
    if args.thumbnail.as_deref() == Some("help") {
        print!("{}", thumbnail::HELP);
        return Ok(true);
    }
    let specs = Specs::resolve(
        &args.preset,
        args.video.as_deref(),
        args.audio.as_deref(),
        args.thumbnail.as_deref(),
    )?;
    let applied = format!(
        "preset: {} (--video \"{}\" --audio \"{}\" --thumbnail \"{}\")",
        specs.preset, specs.video_text, specs.audio_text, specs.thumbnail_text
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
