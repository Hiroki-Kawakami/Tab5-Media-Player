// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::io::Write;

use anyhow::Result;

const EBML: u32 = 0x1A45DFA3;
const EBML_VERSION: u32 = 0x4286;
const EBML_READ_VERSION: u32 = 0x42F7;
const EBML_MAX_ID_LENGTH: u32 = 0x42F2;
const EBML_MAX_SIZE_LENGTH: u32 = 0x42F3;
const DOC_TYPE: u32 = 0x4282;
const DOC_TYPE_VERSION: u32 = 0x4287;
const DOC_TYPE_READ_VERSION: u32 = 0x4285;
const SEGMENT: u32 = 0x18538067;
const INFO: u32 = 0x1549A966;
const TIMESTAMP_SCALE: u32 = 0x2AD7B1;
const MUXING_APP: u32 = 0x4D80;
const WRITING_APP: u32 = 0x5741;
const TRACKS: u32 = 0x1654AE6B;
const TRACK_ENTRY: u32 = 0xAE;
const TRACK_NUMBER: u32 = 0xD7;
const TRACK_UID: u32 = 0x73C5;
const TRACK_TYPE: u32 = 0x83;
const CODEC_ID: u32 = 0x86;
const DEFAULT_DURATION: u32 = 0x23E383;
const VIDEO: u32 = 0xE0;
const PIXEL_WIDTH: u32 = 0xB0;
const PIXEL_HEIGHT: u32 = 0xBA;
const CLUSTER: u32 = 0x1F43B675;
const TIMESTAMP: u32 = 0xE7;
const BLOCK_GROUP: u32 = 0xA0;
const BLOCK: u32 = 0xA1;
const BLOCK_DURATION: u32 = 0x9B;

const UNKNOWN_SIZE: [u8; 8] = [0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
const MICROSECOND_NS: u64 = 1000;
const APP: &[u8] = b"tab5conv";

fn id(out: &mut Vec<u8>, id: u32) {
    out.extend(id.to_be_bytes().into_iter().skip_while(|&b| b == 0));
}

fn size(out: &mut Vec<u8>, size: usize) {
    out.push(0x01);
    out.extend(&(size as u64).to_be_bytes()[1..]);
}

fn element(out: &mut Vec<u8>, element: u32, body: &[u8]) {
    id(out, element);
    size(out, body.len());
    out.extend(body);
}

fn uint(out: &mut Vec<u8>, element: u32, value: u64) {
    let bytes = value.to_be_bytes();
    let skip = bytes.iter().take(7).take_while(|&&b| b == 0).count();
    self::element(out, element, &bytes[skip..]);
}

fn nested(out: &mut Vec<u8>, element: u32, body: impl FnOnce(&mut Vec<u8>)) {
    let mut inner = Vec::new();
    body(&mut inner);
    self::element(out, element, &inner);
}

/// A live Matroska stream (unknown segment size, no cues) with one video
/// track. Every frame is a keyframe with its own time and duration.
pub struct MkvVideoStream<W: Write> {
    out: W,
}

impl<W: Write> MkvVideoStream<W> {
    pub fn new(mut out: W, codec: &str, width: u32, height: u32, frame_ns: u64) -> Result<Self> {
        let mut head = Vec::new();
        nested(&mut head, EBML, |e| {
            uint(e, EBML_VERSION, 1);
            uint(e, EBML_READ_VERSION, 1);
            uint(e, EBML_MAX_ID_LENGTH, 4);
            uint(e, EBML_MAX_SIZE_LENGTH, 8);
            element(e, DOC_TYPE, b"matroska");
            uint(e, DOC_TYPE_VERSION, 4);
            uint(e, DOC_TYPE_READ_VERSION, 2);
        });
        id(&mut head, SEGMENT);
        head.extend(UNKNOWN_SIZE);
        nested(&mut head, INFO, |e| {
            uint(e, TIMESTAMP_SCALE, MICROSECOND_NS);
            element(e, MUXING_APP, APP);
            element(e, WRITING_APP, APP);
        });
        nested(&mut head, TRACKS, |e| {
            nested(e, TRACK_ENTRY, |t| {
                uint(t, TRACK_NUMBER, 1);
                uint(t, TRACK_UID, 1);
                uint(t, TRACK_TYPE, 1);
                element(t, CODEC_ID, codec.as_bytes());
                uint(t, DEFAULT_DURATION, frame_ns);
                nested(t, VIDEO, |v| {
                    uint(v, PIXEL_WIDTH, width.into());
                    uint(v, PIXEL_HEIGHT, height.into());
                });
            });
        });
        out.write_all(&head)?;
        Ok(Self { out })
    }

    pub fn write(&mut self, data: &[u8], time_us: u64, duration_us: u64) -> Result<()> {
        let mut timestamp = Vec::new();
        uint(&mut timestamp, TIMESTAMP, time_us);
        let mut duration = Vec::new();
        uint(&mut duration, BLOCK_DURATION, duration_us);
        let mut block = Vec::new();
        id(&mut block, BLOCK);
        size(&mut block, 4 + data.len());
        block.extend([0x81, 0, 0, 0]);
        let group = duration.len() + block.len() + data.len();

        let mut head = Vec::new();
        id(&mut head, CLUSTER);
        size(&mut head, timestamp.len() + 9 + group);
        head.extend(timestamp);
        id(&mut head, BLOCK_GROUP);
        size(&mut head, group);
        head.extend(duration);
        head.extend(block);
        self.out.write_all(&head)?;
        self.out.write_all(data)?;
        Ok(())
    }

    pub fn into_inner(self) -> W {
        self.out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::container::demux::{self, Codec};

    #[test]
    fn frames_keep_their_times_and_durations() {
        let mut stream = MkvVideoStream::new(Vec::new(), "V_MJPEG", 64, 32, 33_366_667).unwrap();
        let frames: [(&[u8], u64, u64); 3] = [
            (&[1, 2, 3], 0, 100_100),
            (&[4; 300], 100_100, 33_366),
            (&[5], 133_466, 2_000_000),
        ];
        for (data, time, duration) in frames {
            stream.write(data, time, duration).unwrap();
        }
        let file = stream.into_inner();
        let mut demuxer = demux::open(&file[..]).unwrap();
        let (track, video) = demuxer.info().video().unwrap();
        assert_eq!(track.codec, Codec::Mjpeg);
        assert_eq!((video.width, video.height), (64, 32));
        let mut got = Vec::new();
        while let Some(p) = demuxer.next_packet().unwrap() {
            assert!(p.key);
            got.push((p.data, p.pts, p.duration));
        }
        let expected: Vec<(Vec<u8>, i64, i64)> = frames
            .iter()
            .map(|&(d, t, n)| (d.to_vec(), t as i64, n as i64))
            .collect();
        assert_eq!(got, expected);
    }
}
