// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::BTreeMap;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::process::Command;

use tab5conv_core::container::demux::{self, Codec, Packet, TrackKind};
use tab5conv_core::container::mux::{Mp4Muxer, MuxCodec, MuxKind, MuxTrack};
use tab5conv_core::jpeg::Frame;
use tab5conv_core::thumbnail;

fn ffmpeg(args: &[&str]) {
    let status = Command::new("ffmpeg")
        .args(["-hide_banner", "-loglevel", "error", "-y"])
        .args(args)
        .status()
        .expect("ffmpeg must be in PATH");
    assert!(status.success(), "ffmpeg {args:?}");
}

fn make(dir: &Path, name: &str, args: &[&str]) -> PathBuf {
    let path = dir.join(name);
    let mut all = args.to_vec();
    let out = path.to_str().unwrap();
    all.push(out);
    ffmpeg(&all);
    path
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for &x in data {
        a = (a + x as u32) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}

#[derive(Debug, PartialEq)]
struct Expected {
    pts: Option<i64>,
    size: usize,
    key: bool,
    hash: u32,
}

fn ffprobe_packets(path: &Path) -> BTreeMap<u32, Vec<Expected>> {
    let output = Command::new("ffprobe")
        .args(["-v", "error", "-show_data_hash", "adler32", "-show_entries"])
        .arg("packet=stream_index,pts_time,size,flags,data_hash")
        .args(["-of", "csv=p=0"])
        .arg(path)
        .output()
        .unwrap();
    assert!(output.status.success());
    let mut packets: BTreeMap<u32, Vec<Expected>> = BTreeMap::new();
    for line in String::from_utf8(output.stdout).unwrap().lines() {
        let f: Vec<&str> = line.split(',').collect();
        let pts = f[1].parse::<f64>().ok().map(|t| (t * 1e6).round() as i64);
        packets
            .entry(f[0].parse().unwrap())
            .or_default()
            .push(Expected {
                pts,
                size: f[2].parse().unwrap(),
                key: f[3].contains('K'),
                hash: u32::from_str_radix(f[4].trim_start_matches("adler32:"), 16).unwrap(),
            });
    }
    packets
}

fn demux_all(path: &Path) -> (demux::Info, BTreeMap<u32, Vec<Packet>>) {
    let mut demuxer = demux::open(File::open(path).unwrap()).unwrap();
    let info = demuxer.info().clone();
    let mut packets: BTreeMap<u32, Vec<Packet>> = BTreeMap::new();
    while let Some(packet) = demuxer.next_packet().unwrap() {
        packets.entry(packet.track).or_default().push(packet);
    }
    (info, packets)
}

fn check_against_ffprobe(path: &Path) -> demux::Info {
    let (info, ours) = demux_all(path);
    let theirs = ffprobe_packets(path);
    assert_eq!(
        ours.keys().collect::<Vec<_>>(),
        theirs.keys().collect::<Vec<_>>(),
        "{}",
        path.display()
    );
    for (track, expected) in &theirs {
        let got = &ours[track];
        assert_eq!(
            got.len(),
            expected.len(),
            "{} track {track}",
            path.display()
        );
        for (i, (g, e)) in got.iter().zip(expected).enumerate() {
            let what = format!("{} track {track} packet {i}", path.display());
            assert_eq!(g.data.len(), e.size, "{what}");
            assert_eq!(adler32(&g.data), e.hash, "{what}");
            assert_eq!(g.key, e.key, "{what}");
            if let Some(pts) = e.pts {
                assert!((g.pts - pts).abs() <= 1, "{what}: pts {} vs {pts}", g.pts);
            }
        }
    }
    info
}

fn video(info: &demux::Info) -> &tab5conv_core::container::demux::VideoTrack {
    info.video().unwrap().1
}

#[test]
fn mp4_h264_aac_with_b_frames_and_edit_lists() {
    let dir = tempfile::tempdir().unwrap();
    let path = make(
        dir.path(),
        "a.mp4",
        &[
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x240:r=25:d=2",
            "-f",
            "lavfi",
            "-i",
            "sine=r=44100:d=2",
            "-c:v",
            "libx264",
            "-bf",
            "2",
            "-c:a",
            "aac",
            "-ac",
            "2",
        ],
    );
    let info = check_against_ffprobe(&path);
    let (track, v) = info.video().unwrap();
    assert_eq!(track.codec, Codec::H264);
    assert_eq!(track.extradata[0], 1);
    assert!(track.codec_string().unwrap().starts_with("avc1.64"));
    assert_eq!((v.width, v.height, v.rotation), (320, 240, 0));
    assert_eq!(v.frame_rate.map(|r| r.to_string()), Some("25".into()));
    let (track, a) = info.audio().unwrap();
    assert_eq!(track.aac_profile(), Some("LC"));
    assert_eq!((a.channels, a.sample_rate), (2, 44100));
    assert!(a.bit_rate.is_some());
    assert_eq!(a.priming, 1024);
    let index = track.index;
    let (_, packets) = demux_all(&path);
    assert_eq!(packets[&index][0].pts, -23219);
}

#[test]
fn mov_rotation_aspect_and_mp3() {
    let dir = tempfile::tempdir().unwrap();
    let plain = make(
        dir.path(),
        "plain.mov",
        &[
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=720x480:r=30000/1001:d=1",
            "-f",
            "lavfi",
            "-i",
            "sine=r=48000:d=1",
            "-c:v",
            "libx264",
            "-aspect",
            "16:9",
            "-c:a",
            "libmp3lame",
        ],
    );
    for (display_rotation, clockwise) in [("90", 270), ("-90", 90), ("180", 180)] {
        let path = make(
            dir.path(),
            &format!("rot{display_rotation}.mov"),
            &[
                "-display_rotation",
                display_rotation,
                "-i",
                plain.to_str().unwrap(),
                "-c",
                "copy",
            ],
        );
        let info = check_against_ffprobe(&path);
        let v = video(&info);
        assert_eq!(v.rotation, clockwise, "display_rotation {display_rotation}");
        let (w, h) = v.display_size();
        let expected = if clockwise == 180 {
            (853.33, 480.0)
        } else {
            (480.0, 853.33)
        };
        assert!(
            (w - expected.0).abs() < 0.01 && (h - expected.1).abs() < 0.01,
            "{w}x{h}"
        );
        assert_eq!(
            v.frame_rate.map(|r| r.to_string()),
            Some("30000/1001".into())
        );
        assert_eq!(info.audio().unwrap().0.codec, Codec::Mp3);
    }
}

#[test]
fn mkv_and_webm() {
    let dir = tempfile::tempdir().unwrap();
    let cases: [(&str, &[&str], Codec, Codec); 3] = [
        (
            "a.mkv",
            &["-c:v", "libx264", "-bf", "2", "-c:a", "libopus"],
            Codec::H264,
            Codec::Opus,
        ),
        (
            "b.webm",
            &[
                "-c:v",
                "libvpx-vp9",
                "-deadline",
                "realtime",
                "-c:a",
                "libvorbis",
            ],
            Codec::Vp9,
            Codec::Vorbis,
        ),
        (
            "c.mkv",
            &[
                "-c:v",
                "libx265",
                "-x265-params",
                "log-level=none",
                "-c:a",
                "aac",
            ],
            Codec::Hevc,
            Codec::Aac,
        ),
    ];
    for (name, codecs, video_codec, audio_codec) in cases {
        let mut args = vec![
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x240:r=24:d=2",
            "-f",
            "lavfi",
            "-i",
            "sine=r=48000:d=2",
            "-aspect",
            "2:1",
        ];
        args.extend(codecs);
        let path = make(dir.path(), name, &args);
        let info = check_against_ffprobe(&path);
        let (track, v) = info.video().unwrap();
        assert_eq!(track.codec, video_codec, "{name}");
        let codec = track.codec_string().unwrap();
        let prefix = match video_codec {
            Codec::H264 => "avc1.64",
            Codec::Vp9 => "vp09.00.",
            _ => "hvc1.1.6.L",
        };
        assert!(codec.starts_with(prefix), "{name}: {codec}");
        assert_eq!(
            v.frame_rate.map(|r| r.to_string()),
            Some("24".into()),
            "{name}"
        );
        let (w, h) = v.display_size();
        assert!((w - 480.0).abs() < 0.01 && h == 240.0, "{name}: {w}x{h}");
        let (track, a) = info.audio().unwrap();
        assert_eq!(track.codec, audio_codec, "{name}");
        assert!(track.codec_string().is_some(), "{name}");
        assert_eq!(a.sample_rate, 48000, "{name}");
        assert!(!track.extradata.is_empty(), "{name}");
        assert!((info.duration.unwrap() - 2.0).abs() < 0.1, "{name}");
    }
}

fn probe_value(path: &Path, stream: &str, entry: &str) -> String {
    let output = Command::new("ffprobe")
        .args([
            "-v",
            "error",
            "-select_streams",
            stream,
            "-show_entries",
            entry,
        ])
        .args(["-of", "default=nw=1:nk=1"])
        .arg(path)
        .output()
        .unwrap();
    String::from_utf8(output.stdout).unwrap().trim().to_string()
}

#[test]
fn mp4_mux_round_trip() {
    let dir = tempfile::tempdir().unwrap();
    let source = make(
        dir.path(),
        "source.mkv",
        &[
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x240:r=30:d=2",
            "-f",
            "lavfi",
            "-i",
            "sine=r=48000:d=2",
            "-c:v",
            "mjpeg",
            "-c:a",
            "aac",
            "-ac",
            "2",
        ],
    );
    let (info, packets) = demux_all(&source);
    let (video_track, _) = info.video().unwrap();
    let (audio_track, _) = info.audio().unwrap();
    let output = dir.path().join("out.mp4");
    let tracks = vec![
        MuxTrack {
            codec: MuxCodec::Mjpeg,
            kind: MuxKind::Video {
                width: 320,
                height: 240,
                display_rotation: Some(-90),
            },
            timescale: 30,
        },
        MuxTrack {
            codec: MuxCodec::Aac {
                config: audio_track.extradata.clone(),
            },
            kind: MuxKind::Audio {
                channels: 2,
                sample_rate: 48000,
            },
            timescale: 48000,
        },
    ];
    let mut muxer = Mp4Muxer::new(File::create(&output).unwrap(), tracks).unwrap();
    let frames = &packets[&video_track.index];
    let audio = &packets[&audio_track.index];
    let mut a = audio.iter().peekable();
    for (i, frame) in frames.iter().enumerate() {
        while let Some(packet) = a.next_if(|p| p.pts <= (i as i64 + 1) * 1_000_000 / 30) {
            muxer.write(1, &packet.data, 1024, true).unwrap();
        }
        muxer.write(0, &frame.data, 1, true).unwrap();
    }
    for packet in a {
        muxer.write(1, &packet.data, 1024, true).unwrap();
    }
    muxer.set_skip(1, 1024).unwrap();
    let cover = thumbnail::encode(64, 48, &vec![128; Frame::bytes(64, 48)], 85).unwrap();
    muxer.set_cover(cover.clone());
    muxer.finish().unwrap();

    let written = ffprobe_packets(&output);
    let hashes = |list: &[Packet]| list.iter().map(|p| adler32(&p.data)).collect::<Vec<_>>();
    assert_eq!(
        written[&0].iter().map(|e| e.hash).collect::<Vec<_>>(),
        hashes(frames)
    );
    assert_eq!(
        written[&1].iter().map(|e| e.hash).collect::<Vec<_>>(),
        hashes(audio)
    );
    assert_eq!(
        probe_value(&output, "v:0", "stream=codec_name,codec_tag_string"),
        "mjpeg\njpeg"
    );
    assert_eq!(
        probe_value(&output, "v:0", "stream_side_data=rotation"),
        "-90"
    );
    assert_eq!(probe_value(&output, "v:0", "stream=avg_frame_rate"), "30/1");
    assert_eq!(
        probe_value(&output, "a", "stream=codec_name,profile"),
        "aac\nLC"
    );
    // The cover sits in moov/udta/meta/ilst/covr, which ffmpeg reads back as
    // an attached picture rather than a track of its own.
    assert_eq!(
        probe_value(&output, "v:1", "stream=codec_name,width,height"),
        "mjpeg\n64\n48"
    );
    assert_eq!(
        probe_value(&output, "v:1", "stream_disposition=attached_pic"),
        "1"
    );
    assert_eq!(written[&2][0].hash, adler32(&cover));
    let decode = Command::new("ffmpeg")
        .args(["-v", "error", "-i"])
        .arg(&output)
        .args(["-f", "null", "-"])
        .output()
        .unwrap();
    assert!(
        decode.status.success() && decode.stderr.is_empty(),
        "{decode:?}"
    );

    let (again, _) = demux_all(&output);
    assert_eq!(video(&again).rotation, 90);
    assert!(matches!(again.tracks[1].kind, TrackKind::Audio(_)));
}
