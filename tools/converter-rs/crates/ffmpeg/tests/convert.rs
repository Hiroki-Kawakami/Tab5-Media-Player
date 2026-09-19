// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

use tab5conv_core::Specs;
use tab5conv_core::container::Container;
use tab5conv_ffmpeg::{Cancelled, Conversion, Monitor, Tools, batch};

const SECONDS: f64 = 3.0;
const LONG_SECONDS: f64 = 60.0;

fn source(dir: &Path, seconds: f64) -> PathBuf {
    let path = dir.join("source.mp4");
    let status = Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
        ])
        .arg(format!("testsrc2=s=320x240:r=30:d={seconds}"))
        .args(["-f", "lavfi", "-i"])
        .arg(format!("sine=r=48000:d={seconds}"))
        .args(["-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"])
        .arg(&path)
        .status()
        .expect("ffmpeg must be in PATH");
    assert!(status.success());
    path
}

fn prepare(dir: &Path, preset: &str, seconds: f64) -> Conversion {
    let input = source(dir, seconds);
    let output = dir.join("out.mp4");
    let specs = Specs::resolve(preset, None, Some("aac,keep=none")).unwrap();
    Conversion::prepare(&Tools::default(), &input, &output, Container::Mp4, &specs).unwrap()
}

fn run(conversion: &Conversion, cancel_after: Option<f64>) -> (anyhow::Result<()>, Vec<f64>) {
    let seen = Mutex::new(Vec::new());
    let cancel = AtomicBool::new(false);
    let progress = |seconds: f64| {
        seen.lock().unwrap().push(seconds);
        if cancel_after.is_some_and(|after| seconds >= after) {
            cancel.store(true, Ordering::Relaxed);
        }
    };
    let monitor = Monitor {
        progress: &progress,
        cancel: &cancel,
    };
    let result = conversion.run(Some(&monitor)).map(|_| ());
    (result, seen.into_inner().unwrap())
}

fn check_finished(preset: &str) {
    let dir = tempfile::tempdir().unwrap();
    let conversion = prepare(dir.path(), preset, SECONDS);
    assert_eq!(conversion.info.duration.map(f64::round), Some(SECONDS));
    let (result, seen) = run(&conversion, None);
    result.unwrap();
    assert!(!seen.is_empty());
    assert!(seen.windows(2).all(|w| w[0] <= w[1]), "{seen:?}");
    assert!(*seen.last().unwrap() > SECONDS - 0.5, "{seen:?}");
    assert!(conversion.output().is_file());
    assert!(!batch::part_path(conversion.output()).exists());
}

fn check_cancelled(preset: &str) {
    let dir = tempfile::tempdir().unwrap();
    let conversion = prepare(dir.path(), preset, LONG_SECONDS);
    let (result, seen) = run(&conversion, Some(0.1));
    assert!(*seen.last().unwrap() < LONG_SECONDS / 2.0, "{seen:?}");
    let err = result.unwrap_err();
    assert!(err.is::<Cancelled>(), "{err:#}");
    assert!(!conversion.output().exists());
    assert!(!batch::part_path(conversion.output()).exists());
}

#[test]
fn ffmpeg_encode_reports_progress() {
    check_finished("tiny");
}

#[test]
fn mjpeg_pipeline_reports_progress() {
    check_finished("default");
}

#[test]
fn ffmpeg_encode_can_be_cancelled() {
    check_cancelled("tiny");
}

#[test]
fn mjpeg_pipeline_can_be_cancelled() {
    check_cancelled("default");
}
