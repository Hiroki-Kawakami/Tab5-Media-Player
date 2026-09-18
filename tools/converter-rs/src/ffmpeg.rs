// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::ffi::{OsStr, OsString};
use std::io::ErrorKind;
use std::path::Path;
use std::process::{Command, Output};

use anyhow::{Context, Result, bail};

const FFMPEG: &str = "ffmpeg";
const FFPROBE: &str = "ffprobe";

fn capture(program: &str, args: &[&OsStr]) -> Result<Output> {
    let output = Command::new(program).args(args).output().map_err(|e| {
        if e.kind() == ErrorKind::NotFound {
            anyhow::anyhow!("{program} not found in PATH; install ffmpeg first")
        } else {
            anyhow::Error::new(e).context(format!("failed to run {program}"))
        }
    })?;
    if !output.status.success() {
        bail!(
            "{program} failed ({}): {}",
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }
    Ok(output)
}

pub fn require_encoders(names: &[&str]) -> Result<()> {
    let output = capture(FFMPEG, &["-hide_banner".as_ref(), "-encoders".as_ref()])?;
    let listing = String::from_utf8_lossy(&output.stdout);
    let available: Vec<&str> = listing
        .lines()
        .filter_map(|line| line.split_whitespace().nth(1))
        .collect();
    let missing: Vec<&str> = names
        .iter()
        .copied()
        .filter(|name| !available.contains(name))
        .collect();
    if !missing.is_empty() {
        bail!("this ffmpeg has no encoder for: {}", missing.join(", "));
    }
    Ok(())
}

pub fn probe_json(input: &Path) -> Result<String> {
    let args: [&OsStr; 7] = [
        "-v".as_ref(),
        "error".as_ref(),
        "-print_format".as_ref(),
        "json".as_ref(),
        "-show_streams".as_ref(),
        "-show_format".as_ref(),
        input.as_os_str(),
    ];
    let output = capture(FFPROBE, &args)?;
    String::from_utf8(output.stdout).context("ffprobe printed non-UTF-8 output")
}

pub fn run(args: &[OsString]) -> Result<()> {
    let status = Command::new(FFMPEG)
        .args(args)
        .status()
        .with_context(|| format!("failed to run {FFMPEG}"))?;
    if !status.success() {
        bail!("{FFMPEG} failed ({status})");
    }
    Ok(())
}

fn quote(arg: &str) -> String {
    let plain = !arg.is_empty()
        && arg
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "_-+=:,./@%".contains(c));
    if plain {
        arg.to_string()
    } else {
        format!("'{}'", arg.replace('\'', r"'\''"))
    }
}

pub fn command_line(args: &[OsString]) -> String {
    std::iter::once(FFMPEG.to_string())
        .chain(args.iter().map(|a| quote(&a.to_string_lossy())))
        .collect::<Vec<_>>()
        .join(" ")
}
