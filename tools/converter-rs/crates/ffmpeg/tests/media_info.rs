// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::fs::File;
use std::path::{Path, PathBuf};
use std::process::Command;

use tab5conv_core::Specs;
use tab5conv_core::container::demux;
use tab5conv_core::media::MediaInfo;
use tab5conv_core::preset;
use tab5conv_ffmpeg::{Tools, probe};

fn make(dir: &Path, name: &str, args: &[&str]) -> PathBuf {
    let path = dir.join(name);
    let status = Command::new("ffmpeg")
        .args(["-hide_banner", "-loglevel", "error", "-y"])
        .args(args)
        .arg(&path)
        .status()
        .expect("ffmpeg must be in PATH");
    assert!(status.success(), "{name}");
    path
}

fn labels(info: &MediaInfo, video: Option<&str>, audio: Option<&str>) -> Vec<String> {
    preset::all()
        .iter()
        .map(|p| {
            let specs = match Specs::resolve(p.name, video, audio) {
                Ok(specs) => specs,
                Err(err) => return format!("{}: {err:#}", p.name),
            };
            let video = specs
                .video
                .plan(&info.video)
                .map_or_else(|e| format!("{e:#}"), |p| p.label);
            let audio = specs
                .audio
                .plan(info.audio.as_ref())
                .map_or_else(|e| format!("{e:#}"), |p| p.label);
            format!("{}: {video} / {audio}", p.name)
        })
        .collect()
}

fn compare(path: &Path) {
    let name = path.file_name().unwrap().to_string_lossy();
    let theirs = probe(&Tools::default(), path).unwrap();
    let ours = demux::open(File::open(path).unwrap())
        .unwrap()
        .info()
        .media_info()
        .unwrap();
    let close = |a: f64, b: f64, tolerance: f64| (a - b).abs() <= tolerance;
    assert!(
        close(ours.duration.unwrap(), theirs.duration.unwrap(), 0.05),
        "{name}: duration {:?} vs {:?}",
        ours.duration,
        theirs.duration
    );
    let (v, t) = (&ours.video, &theirs.video);
    assert!(
        close(v.display_width, t.display_width, 0.01)
            && close(v.display_height, t.display_height, 0.01),
        "{name}: {}x{} vs {}x{}",
        v.display_width,
        v.display_height,
        t.display_width,
        t.display_height
    );
    assert!(
        close(v.fps.unwrap().as_f64(), t.fps.unwrap().as_f64(), 1e-3),
        "{name}: fps {:?} vs {:?}",
        v.fps,
        t.fps
    );
    match (&ours.audio, &theirs.audio) {
        (None, None) => {}
        (Some(a), Some(t)) => {
            assert_eq!(
                (&a.codec_name, &a.profile, a.channels, a.sample_rate),
                (&t.codec_name, &t.profile, t.channels, t.sample_rate),
                "{name}"
            );
            match (a.bit_rate, t.bit_rate) {
                (Some(x), Some(y)) => assert!(
                    close(x as f64, y as f64, y as f64 * 0.01),
                    "{name}: {x} vs {y}"
                ),
                (x, y) => assert_eq!(x, y, "{name}: bit rate"),
            }
        }
        _ => panic!("{name}: audio presence differs"),
    }
    for (video, audio) in [
        (None, None),
        (Some("mjpeg,rotatewhen=always"), Some("mp3")),
        (Some("h264,fps=24"), Some("aac,bitrate=96k")),
    ] {
        assert_eq!(
            labels(&ours, video, audio),
            labels(&theirs, video, audio),
            "{name}"
        );
    }
}

#[test]
fn demuxed_info_matches_ffprobe() {
    let dir = tempfile::tempdir().unwrap();
    let d = dir.path();
    let src = |size: &str, rate: &str, secs: &str| format!("testsrc2=s={size}:r={rate}:d={secs}");
    let sine = |rate: &str, secs: &str| format!("sine=r={rate}:d={secs}");
    let files = [
        make(
            d,
            "a.mp4",
            &[
                "-f",
                "lavfi",
                "-i",
                &src("1920x1080", "30000/1001", "2"),
                "-f",
                "lavfi",
                "-i",
                &sine("48000", "2"),
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-c:a",
                "aac",
                "-b:a",
                "128k",
            ],
        ),
        make(
            d,
            "b.mov",
            &[
                "-f",
                "lavfi",
                "-i",
                &src("720x480", "25", "2"),
                "-f",
                "lavfi",
                "-i",
                &sine("44100", "2"),
                "-aspect",
                "16:9",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-c:a",
                "libmp3lame",
                "-b:a",
                "160k",
            ],
        ),
        make(
            d,
            "c.mkv",
            &[
                "-f",
                "lavfi",
                "-i",
                &src("1080x1920", "60", "2"),
                "-f",
                "lavfi",
                "-i",
                &sine("48000", "2"),
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-c:a",
                "libopus",
            ],
        ),
        make(
            d,
            "d.webm",
            &[
                "-f",
                "lavfi",
                "-i",
                &src("640x360", "24", "2"),
                "-f",
                "lavfi",
                "-i",
                &sine("48000", "2"),
                "-c:v",
                "libvpx-vp9",
                "-deadline",
                "realtime",
                "-c:a",
                "libvorbis",
            ],
        ),
        make(
            d,
            "e.mkv",
            &[
                "-f",
                "lavfi",
                "-i",
                &src("320x240", "15", "2"),
                "-f",
                "lavfi",
                "-i",
                &sine("48000", "2"),
                "-ac",
                "6",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-c:a",
                "aac",
            ],
        ),
        make(
            d,
            "f.mp4",
            &[
                "-f",
                "lavfi",
                "-i",
                &src("640x360", "24", "2"),
                "-f",
                "lavfi",
                "-i",
                &sine("8000", "2"),
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-c:a",
                "aac",
            ],
        ),
        make(
            d,
            "g.mp4",
            &[
                "-f",
                "lavfi",
                "-i",
                &src("640x360", "30", "2"),
                "-c:v",
                "libx265",
                "-x265-params",
                "log-level=none",
            ],
        ),
    ];
    let rotated = make(
        d,
        "h.mov",
        &[
            "-display_rotation",
            "90",
            "-i",
            files[1].to_str().unwrap(),
            "-c",
            "copy",
        ],
    );
    for file in files.iter().chain([&rotated]) {
        compare(file);
    }
}
