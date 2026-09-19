// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::fs::File;
use std::path::Path;
use std::process::Command;

use tab5conv_core::container::mux::{Mp4Muxer, MuxCodec, MuxKind, MuxTrack};
use tab5conv_core::framerate::Rate;
use tab5conv_core::jpeg::Frame;
use tab5conv_core::mpeg2::{self, GopEncoder, Matrix, PictureType, Settings};

const WIDTH: usize = 200;
const HEIGHT: usize = 116;
const FRAMES: usize = 47;
const KEYINT: usize = 20;

fn ffmpeg(args: &[&str]) -> Vec<u8> {
    let output = Command::new("ffmpeg")
        .args(["-hide_banner", "-loglevel", "error", "-y"])
        .args(args)
        .output()
        .expect("ffmpeg must be in PATH");
    assert!(output.status.success(), "ffmpeg {args:?}");
    output.stdout
}

fn probe(path: &Path, entries: &str) -> String {
    let output = Command::new("ffprobe")
        .args([
            "-v",
            "error",
            "-select_streams",
            "v",
            "-show_entries",
            entries,
        ])
        .args(["-of", "csv=p=0"])
        .arg(path)
        .output()
        .unwrap();
    String::from_utf8(output.stdout).unwrap().trim().to_string()
}

struct Encoded {
    gop: usize,
    index: usize,
    kind: PictureType,
    data: Vec<u8>,
}

fn encode(settings: &Settings, source: &[u8]) -> (Vec<Encoded>, Vec<u8>) {
    let size = Frame::bytes(WIDTH, HEIGHT);
    let mut pictures = Vec::new();
    let mut recon = vec![Vec::new(); FRAMES];
    let mut keep = |gop: usize, list: Vec<mpeg2::Picture>| {
        for p in list {
            recon[gop * KEYINT + p.index] = p.recon.unwrap();
            pictures.push(Encoded {
                gop,
                index: p.index,
                kind: p.kind,
                data: p.data,
            });
        }
    };
    for (gop, frames) in source.chunks(size * KEYINT).enumerate() {
        let mut encoder = GopEncoder::new(settings, (gop * KEYINT) as u64).keep_recon();
        for data in frames.chunks(size) {
            let frame = Frame::from_yuv420p(data, WIDTH, HEIGHT);
            keep(gop, encoder.push(&frame).unwrap());
        }
        keep(gop, encoder.finish());
    }
    (pictures, recon.concat())
}

fn decode(args: &[&str]) -> Vec<u8> {
    let mut all = vec!["-idct", "simple"];
    all.extend(args);
    all.extend(["-f", "rawvideo", "-pix_fmt", "yuv420p", "-"]);
    ffmpeg(&all)
}

#[test]
fn reconstruction_matches_ffmpeg_and_muxes_with_reordering() {
    let dir = tempfile::tempdir().unwrap();
    let source = ffmpeg(&[
        "-f",
        "lavfi",
        "-i",
        &format!("testsrc2=s={WIDTH}x{HEIGHT}:r=30"),
        "-frames:v",
        &FRAMES.to_string(),
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv420p",
        "-",
    ]);
    for (bframes, hq) in [(2, true), (0, false), (3, false)] {
        let settings = Settings {
            width: WIDTH,
            height: HEIGHT,
            rate: Rate::new(30, 1).unwrap(),
            qscale: 6,
            bframes,
            hq,
            matrix: Some(Matrix::Bt709),
        };
        let (pictures, recon) = encode(&settings, &source);
        let elementary = dir.path().join("out.m2v");
        std::fs::write(
            &elementary,
            pictures
                .iter()
                .flat_map(|p| p.data.clone())
                .collect::<Vec<_>>(),
        )
        .unwrap();
        assert!(
            decode(&["-i", elementary.to_str().unwrap()]) == recon,
            "{bframes} {hq}"
        );

        let timescale = mpeg2::timescale(settings.rate);
        let duration = timescale / 30;
        let delay = mpeg2::reorder_delay(bframes) as u32;
        let output = dir.path().join("out.mp4");
        let track = MuxTrack {
            codec: MuxCodec::Mpeg2 {
                header: mpeg2::sequence_header(&settings),
            },
            kind: MuxKind::Video {
                width: WIDTH as u32,
                height: HEIGHT as u32,
                display_rotation: None,
            },
            timescale,
        };
        let mut muxer = Mp4Muxer::new(File::create(&output).unwrap(), vec![track]).unwrap();
        for (coded, p) in pictures.iter().enumerate() {
            let shown = (p.gop * KEYINT + p.index) as u32;
            let offset = (shown + delay - coded as u32) * duration;
            let key = p.kind == PictureType::I;
            muxer
                .write_with_offset(0, &p.data, duration, key, offset)
                .unwrap();
        }
        muxer.set_skip(0, delay * duration).unwrap();
        muxer.finish().unwrap();
        assert_eq!(
            probe(
                &output,
                "stream=codec_name,codec_tag_string,nb_frames,avg_frame_rate,color_space"
            ),
            format!("mpeg2video,mp4v,bt709,30/1,{FRAMES},")
        );
        let pts: Vec<i64> = probe(&output, "frame=pts")
            .lines()
            .map(|l| l.trim_end_matches(',').parse().unwrap())
            .collect();
        let expected: Vec<i64> = (0..FRAMES as i64).map(|i| i * duration as i64).collect();
        assert_eq!(pts, expected);
        let keys = probe(&output, "packet=flags").matches('K').count();
        assert_eq!(keys, FRAMES.div_ceil(KEYINT));
        assert!(decode(&["-i", output.to_str().unwrap()]) == recon);
    }
}
