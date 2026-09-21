// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::VecDeque;
use std::env;
use std::ffi::{OsStr, OsString};
use std::fmt;
use std::io::{self, BufRead, BufReader, ErrorKind, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStderr, Command, ExitStatus, Output, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use anyhow::{Context, Result, bail};

const LOG_LINES: usize = 20;
const POLL_INTERVAL: Duration = Duration::from_millis(50);

#[derive(Clone, Debug)]
pub struct Tools {
    ffmpeg: PathBuf,
    ffprobe: PathBuf,
}

impl Default for Tools {
    fn default() -> Self {
        Self {
            ffmpeg: "ffmpeg".into(),
            ffprobe: "ffprobe".into(),
        }
    }
}

fn executable(name: &str) -> String {
    format!("{name}{}", env::consts::EXE_SUFFIX)
}

fn launch_error(program: &Path, err: io::Error) -> anyhow::Error {
    if err.kind() != ErrorKind::NotFound {
        return anyhow::Error::new(err).context(format!("failed to run {}", program.display()));
    }
    if program.components().count() == 1 {
        anyhow::anyhow!(
            "{} not found in PATH; install ffmpeg first",
            program.display()
        )
    } else {
        anyhow::anyhow!("{} not found", program.display())
    }
}

impl Tools {
    pub fn in_dir(dir: &Path) -> Self {
        Self {
            ffmpeg: dir.join(executable("ffmpeg")),
            ffprobe: dir.join(executable("ffprobe")),
        }
    }

    pub fn locate(extra_dirs: &[PathBuf]) -> Option<Self> {
        let path = env::var_os("PATH").unwrap_or_default();
        env::split_paths(&path)
            .chain(extra_dirs.iter().cloned())
            .find(|dir| {
                dir.join(executable("ffmpeg")).is_file()
                    && dir.join(executable("ffprobe")).is_file()
            })
            .map(|dir| Self::in_dir(&dir))
    }

    pub fn ffmpeg(&self) -> &Path {
        &self.ffmpeg
    }

    pub fn version(&self) -> Result<String> {
        let output = capture(&self.ffmpeg, &["-version".as_ref()])?;
        let text = String::from_utf8_lossy(&output.stdout);
        let first = text.lines().next().unwrap_or_default();
        Ok(
            match first.split_whitespace().collect::<Vec<_>>().as_slice() {
                [name, "version", version, ..] => format!("{name} {version}"),
                _ => first.to_string(),
            },
        )
    }

    pub fn require_encoders(&self, names: &[&str]) -> Result<()> {
        let output = capture(
            &self.ffmpeg,
            &["-hide_banner".as_ref(), "-encoders".as_ref()],
        )?;
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

    pub fn probe_json(&self, input: &Path) -> Result<String> {
        let args: [&OsStr; 7] = [
            "-v".as_ref(),
            "error".as_ref(),
            "-print_format".as_ref(),
            "json".as_ref(),
            "-show_streams".as_ref(),
            "-show_format".as_ref(),
            input.as_os_str(),
        ];
        let output = capture(&self.ffprobe, &args)?;
        String::from_utf8(output.stdout).context("ffprobe printed non-UTF-8 output")
    }

    pub fn spawn(
        &self,
        args: &[OsString],
        stdin: Stdio,
        stdout: Stdio,
        stderr: Stdio,
    ) -> Result<Child> {
        Command::new(&self.ffmpeg)
            .args(args)
            .stdin(stdin)
            .stdout(stdout)
            .stderr(stderr)
            .spawn()
            .map_err(|e| launch_error(&self.ffmpeg, e))
    }

    pub fn output(&self, args: &[OsString]) -> Result<Vec<u8>> {
        let output = Command::new(&self.ffmpeg)
            .args(args)
            .stdin(Stdio::null())
            .output()
            .map_err(|e| launch_error(&self.ffmpeg, e))?;
        if !output.status.success() {
            bail!(
                "{}",
                failure(
                    &self.ffmpeg.display().to_string(),
                    output.status,
                    &String::from_utf8_lossy(&output.stderr)
                )
            );
        }
        Ok(output.stdout)
    }

    pub fn run(&self, args: &[OsString]) -> Result<()> {
        let status = Command::new(&self.ffmpeg)
            .args(args)
            .status()
            .map_err(|e| launch_error(&self.ffmpeg, e))?;
        if !status.success() {
            bail!("{} failed ({status})", self.ffmpeg.display());
        }
        Ok(())
    }

    pub fn run_monitored(&self, args: &[OsString], monitor: &Monitor, fps: f64) -> Result<()> {
        let mut child = self.spawn(args, Stdio::null(), Stdio::piped(), Stdio::piped())?;
        let log = Log::capture(child.stderr.take().context("ffmpeg has no stderr")?);
        let progress = child.stdout.take().context("ffmpeg has no stdout")?;
        let status = thread::scope(|scope| {
            scope.spawn(|| read_progress(progress, monitor.progress, fps));
            wait(&mut child, monitor.cancel)
        });
        let log = log.finish();
        let status = status?;
        if !status.success() {
            bail!(
                "{}",
                failure(&self.ffmpeg.display().to_string(), status, &log)
            );
        }
        Ok(())
    }

    pub fn command_line(&self, args: &[OsString]) -> String {
        std::iter::once(quote(&self.ffmpeg.to_string_lossy()))
            .chain(args.iter().map(|a| quote(&a.to_string_lossy())))
            .collect::<Vec<_>>()
            .join(" ")
    }
}

fn capture(program: &Path, args: &[&OsStr]) -> Result<Output> {
    let output = Command::new(program)
        .args(args)
        .output()
        .map_err(|e| launch_error(program, e))?;
    if !output.status.success() {
        bail!(
            "{} failed ({}): {}",
            program.display(),
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }
    Ok(output)
}

pub struct Monitor<'a> {
    pub progress: &'a (dyn Fn(f64) + Sync),
    pub cancel: &'a AtomicBool,
}

#[derive(Debug)]
pub struct Cancelled;

impl fmt::Display for Cancelled {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("cancelled")
    }
}

impl std::error::Error for Cancelled {}

pub fn wait(child: &mut Child, cancel: &AtomicBool) -> Result<ExitStatus> {
    loop {
        if cancel.load(Ordering::Relaxed) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(Cancelled.into());
        }
        if let Some(status) = child.try_wait().context("waiting for ffmpeg")? {
            return Ok(status);
        }
        thread::sleep(POLL_INTERVAL);
    }
}

/// ffmpeg's `out_time` is the time of whatever stream it muxed last, and cover
/// art is muxed after the final video packet: with one attached, the last
/// report of a run says 0.04 s. `frame` only ever counts the first video
/// stream, so the position comes from it and the output's frame rate.
fn read_progress(source: impl Read, progress: &(dyn Fn(f64) + Sync), fps: f64) {
    for line in BufReader::new(source).lines() {
        let Ok(line) = line else { return };
        let seconds = if fps > 0.0 {
            line.strip_prefix("frame=")
                .and_then(|v| v.trim().parse::<u64>().ok())
                .map(|frames| frames as f64 / fps)
        } else {
            line.strip_prefix("out_time_us=")
                .and_then(|v| v.parse::<i64>().ok())
                .filter(|&us| us >= 0)
                .map(|us| us as f64 / 1e6)
        };
        if let Some(seconds) = seconds {
            progress(seconds);
        }
    }
}

pub fn failure(program: &str, status: ExitStatus, log: &str) -> String {
    if log.is_empty() {
        format!("{program} failed ({status})")
    } else {
        format!("{program} failed ({status}): {log}")
    }
}

pub struct Log {
    lines: Arc<Mutex<VecDeque<String>>>,
    thread: JoinHandle<()>,
}

impl Log {
    pub fn capture(stderr: ChildStderr) -> Self {
        let lines = Arc::new(Mutex::new(VecDeque::new()));
        let sink = Arc::clone(&lines);
        let thread = thread::spawn(move || {
            let mut reader = BufReader::new(stderr);
            let mut line = Vec::new();
            while matches!(reader.read_until(b'\n', &mut line), Ok(n) if n > 0) {
                let text = String::from_utf8_lossy(&line).trim().to_string();
                line.clear();
                if text.is_empty() {
                    continue;
                }
                let mut lines = sink.lock().expect("log lock");
                if lines.len() == LOG_LINES {
                    lines.pop_front();
                }
                lines.push_back(text);
            }
        });
        Self { lines, thread }
    }

    pub fn finish(self) -> String {
        let _ = self.thread.join();
        let lines = self.lines.lock().expect("log lock");
        Vec::from_iter(lines.iter().cloned()).join("\n")
    }
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
