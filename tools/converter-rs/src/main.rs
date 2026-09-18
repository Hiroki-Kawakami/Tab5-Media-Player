// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod ffmpeg;
mod plan;
mod probe;

use std::path::{Path, PathBuf};

use anyhow::{Result, bail};
use clap::Parser;

#[derive(Parser)]
#[command(
    version,
    about = "Convert video files for Tab5-Media-Player (H.264 + AAC)"
)]
struct Args {
    input: PathBuf,

    /// Output file (.mp4 or .mkv) [default: <input>.tab5.mp4]
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Longest side of the output picture; smaller sources are not upscaled
    #[arg(long, default_value_t = 640, value_parser = clap::value_parser!(u32).range(16..=plan::MAX_SIDE as i64))]
    long_side: u32,

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

fn main() -> Result<()> {
    let args = Args::parse();
    let output = args
        .output
        .clone()
        .unwrap_or_else(|| default_output(&args.input));
    let container = plan::Container::from_path(&output)?;

    if !args.dry_run && !args.overwrite && output.exists() {
        bail!("{} already exists (use -y to overwrite)", output.display());
    }

    ffmpeg::require_encoders(&["libx264", "aac"])?;
    let info = probe::probe(&args.input)?;
    let plan = plan::Plan::new(&info, args.long_side, container);

    eprintln!(
        "video {}x{} -> {}x{} H.264, audio {}",
        info.video.display_width.round(),
        info.video.display_height.round(),
        plan.width,
        plan.height,
        if info.audio.is_some() { "AAC" } else { "none" },
    );

    let ffmpeg_args = plan.ffmpeg_args(&args.input, &output, args.overwrite);
    if args.dry_run {
        println!("{}", ffmpeg::command_line(&ffmpeg_args));
        return Ok(());
    }
    ffmpeg::run(&ffmpeg_args)
}
