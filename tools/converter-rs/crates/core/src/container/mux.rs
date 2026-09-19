// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Result, bail};

use super::io::Sink;

const MOVIE_TIMESCALE: u32 = 1000;
const MDAT_HEADER: u64 = 16;
const LANGUAGE_UND: u16 = 0x55C4;
const FIXED_ONE: u32 = 0x0001_0000;
const FIXED_MINUS_ONE: u32 = 0xFFFF_0000;
const MATRIX_W: u32 = 0x4000_0000;

#[derive(Clone, Debug, PartialEq)]
pub enum MuxCodec {
    Mjpeg,
    Mpeg2 { header: Vec<u8> },
    Aac { config: Vec<u8> },
    Mp3,
}

#[derive(Clone, Debug, PartialEq)]
pub enum MuxKind {
    Video {
        width: u32,
        height: u32,
        display_rotation: Option<i32>,
    },
    Audio {
        channels: u32,
        sample_rate: u32,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct MuxTrack {
    pub codec: MuxCodec,
    pub kind: MuxKind,
    pub timescale: u32,
}

struct TrackState {
    config: MuxTrack,
    sizes: Vec<u32>,
    durations: Vec<(u32, u32)>,
    offsets: Vec<(u32, u32)>,
    unsynced: Vec<u32>,
    chunks: Vec<(u64, u32)>,
    bytes: u64,
    duration: u64,
    end: u64,
    skip: u32,
}

impl TrackState {
    fn presented(&self) -> u64 {
        self.end.saturating_sub(self.skip as u64)
    }
}

pub struct Mp4Muxer<S: Sink> {
    sink: S,
    tracks: Vec<TrackState>,
    pos: u64,
    mdat_start: u64,
    last_track: Option<usize>,
}

struct Writer(Vec<u8>);

impl Writer {
    fn u8(&mut self, v: u8) {
        self.0.push(v);
    }
    fn u16(&mut self, v: u16) {
        self.0.extend(v.to_be_bytes());
    }
    fn u24(&mut self, v: u32) {
        self.0.extend(&v.to_be_bytes()[1..]);
    }
    fn u32(&mut self, v: u32) {
        self.0.extend(v.to_be_bytes());
    }
    fn u64(&mut self, v: u64) {
        self.0.extend(v.to_be_bytes());
    }
    fn bytes(&mut self, v: &[u8]) {
        self.0.extend(v);
    }
    fn zeros(&mut self, n: usize) {
        self.0.resize(self.0.len() + n, 0);
    }

    fn boxed(&mut self, kind: &[u8; 4], body: impl FnOnce(&mut Self)) {
        let start = self.0.len();
        self.u32(0);
        self.bytes(kind);
        body(self);
        let size = (self.0.len() - start) as u32;
        self.0[start..start + 4].copy_from_slice(&size.to_be_bytes());
    }

    fn full(&mut self, kind: &[u8; 4], version: u8, flags: u32, body: impl FnOnce(&mut Self)) {
        self.boxed(kind, |w| {
            w.u8(version);
            w.u24(flags);
            body(w);
        });
    }

    fn descriptor(&mut self, tag: u8, body: impl FnOnce(&mut Self)) {
        let mut inner = Writer(Vec::new());
        body(&mut inner);
        let len = inner.0.len() as u32;
        self.u8(tag);
        for shift in [21, 14, 7] {
            self.u8(0x80 | ((len >> shift) & 0x7F) as u8);
        }
        self.u8((len & 0x7F) as u8);
        self.bytes(&inner.0);
    }

    fn matrix(&mut self, [a, b, c, d]: [u32; 4]) {
        for v in [a, b, 0, c, d, 0, 0, 0, MATRIX_W] {
            self.u32(v);
        }
    }
}

fn rotation_matrix(display_rotation: Option<i32>) -> [u32; 4] {
    match (-display_rotation.unwrap_or(0)).rem_euclid(360) {
        90 => [0, FIXED_ONE, FIXED_MINUS_ONE, 0],
        180 => [FIXED_MINUS_ONE, 0, 0, FIXED_MINUS_ONE],
        270 => [0, FIXED_MINUS_ONE, FIXED_ONE, 0],
        _ => [FIXED_ONE, 0, 0, FIXED_ONE],
    }
}

fn scale(value: u64, from: u32, to: u32) -> u64 {
    (value as u128 * to as u128 / from.max(1) as u128) as u64
}

impl<S: Sink> Mp4Muxer<S> {
    pub fn new(mut sink: S, tracks: Vec<MuxTrack>) -> Result<Self> {
        if tracks.iter().any(|t| t.timescale == 0) {
            bail!("track timescale must not be zero");
        }
        let mut w = Writer(Vec::new());
        w.boxed(b"ftyp", |w| {
            w.bytes(b"isom");
            w.u32(0x200);
            w.bytes(b"isomiso2mp41");
        });
        let mdat_start = w.0.len() as u64;
        w.u32(1);
        w.bytes(b"mdat");
        w.u64(MDAT_HEADER);
        sink.write(&w.0)?;
        Ok(Self {
            sink,
            tracks: tracks
                .into_iter()
                .map(|config| TrackState {
                    config,
                    sizes: Vec::new(),
                    durations: Vec::new(),
                    offsets: Vec::new(),
                    unsynced: Vec::new(),
                    chunks: Vec::new(),
                    bytes: 0,
                    duration: 0,
                    end: 0,
                    skip: 0,
                })
                .collect(),
            pos: w.0.len() as u64,
            mdat_start,
            last_track: None,
        })
    }

    pub fn set_codec(&mut self, track: usize, codec: MuxCodec) -> Result<()> {
        let Some(state) = self.tracks.get_mut(track) else {
            bail!("no track {track}");
        };
        state.config.codec = codec;
        Ok(())
    }

    pub fn set_skip(&mut self, track: usize, skip: u32) -> Result<()> {
        let Some(state) = self.tracks.get_mut(track) else {
            bail!("no track {track}");
        };
        state.skip = skip;
        Ok(())
    }

    pub fn write(&mut self, track: usize, data: &[u8], duration: u32, key: bool) -> Result<()> {
        self.write_with_offset(track, data, duration, key, 0)
    }

    pub fn write_with_offset(
        &mut self,
        track: usize,
        data: &[u8],
        duration: u32,
        key: bool,
        offset: u32,
    ) -> Result<()> {
        let Some(state) = self.tracks.get_mut(track) else {
            bail!("no track {track}");
        };
        let size = u32::try_from(data.len())?;
        self.sink.write(data)?;
        if self.last_track == Some(track) {
            state.chunks.last_mut().expect("chunk started").1 += 1;
        } else {
            state.chunks.push((self.pos, 1));
            self.last_track = Some(track);
        }
        if !key {
            state.unsynced.push(state.sizes.len() as u32 + 1);
        }
        state.sizes.push(size);
        match state.durations.last_mut() {
            Some((count, delta)) if *delta == duration => *count += 1,
            _ => state.durations.push((1, duration)),
        }
        match state.offsets.last_mut() {
            Some((count, value)) if *value == offset => *count += 1,
            _ => state.offsets.push((1, offset)),
        }
        state.bytes += size as u64;
        state.end = state
            .end
            .max(state.duration + offset as u64 + duration as u64);
        state.duration += duration as u64;
        self.pos += size as u64;
        Ok(())
    }

    pub fn finish(mut self) -> Result<S> {
        let mdat_size = self.pos - self.mdat_start;
        self.sink
            .write_at(self.mdat_start + 8, &mdat_size.to_be_bytes())?;
        let moov = self.moov();
        self.sink.write(&moov)?;
        Ok(self.sink)
    }

    fn moov(&self) -> Vec<u8> {
        let movie_duration = self
            .tracks
            .iter()
            .map(|t| scale(t.presented(), t.config.timescale, MOVIE_TIMESCALE))
            .max()
            .unwrap_or(0);
        let long = self.pos > u32::MAX as u64;
        let mut w = Writer(Vec::new());
        w.boxed(b"moov", |w| {
            w.full(b"mvhd", 0, 0, |w| {
                w.zeros(8);
                w.u32(MOVIE_TIMESCALE);
                w.u32(movie_duration as u32);
                w.u32(FIXED_ONE);
                w.u16(0x0100);
                w.zeros(10);
                w.matrix([FIXED_ONE, 0, 0, FIXED_ONE]);
                w.zeros(24);
                w.u32(self.tracks.len() as u32 + 1);
            });
            for (i, track) in self.tracks.iter().enumerate() {
                trak(w, i as u32 + 1, track, long);
            }
        });
        w.0
    }
}

fn trak(w: &mut Writer, id: u32, track: &TrackState, long: bool) {
    let config = &track.config;
    let movie_duration = scale(track.presented(), config.timescale, MOVIE_TIMESCALE);
    let (video, width, height, rotation) = match config.kind {
        MuxKind::Video {
            width,
            height,
            display_rotation,
        } => (true, width, height, display_rotation),
        MuxKind::Audio { .. } => (false, 0, 0, None),
    };
    w.boxed(b"trak", |w| {
        w.full(b"tkhd", 0, 3, |w| {
            w.zeros(8);
            w.u32(id);
            w.zeros(4);
            w.u32(movie_duration as u32);
            w.zeros(8);
            w.u16(0);
            w.u16(if video { 0 } else { 1 });
            w.u16(if video { 0 } else { 0x0100 });
            w.zeros(2);
            w.matrix(rotation_matrix(rotation));
            w.u32(width << 16);
            w.u32(height << 16);
        });
        if track.skip > 0 {
            w.boxed(b"edts", |w| {
                w.full(b"elst", 0, 0, |w| {
                    w.u32(1);
                    w.u32(movie_duration as u32);
                    w.u32(track.skip);
                    w.u32(FIXED_ONE);
                });
            });
        }
        w.boxed(b"mdia", |w| {
            let version = u8::from(track.duration > u32::MAX as u64);
            w.full(b"mdhd", version, 0, |w| {
                if version == 1 {
                    w.zeros(16);
                    w.u32(config.timescale);
                    w.u64(track.duration);
                } else {
                    w.zeros(8);
                    w.u32(config.timescale);
                    w.u32(track.duration as u32);
                }
                w.u16(LANGUAGE_UND);
                w.u16(0);
            });
            w.full(b"hdlr", 0, 0, |w| {
                w.zeros(4);
                w.bytes(if video { b"vide" } else { b"soun" });
                w.zeros(12);
                w.bytes(if video {
                    b"VideoHandler\0"
                } else {
                    b"SoundHandler\0"
                });
            });
            w.boxed(b"minf", |w| {
                if video {
                    w.full(b"vmhd", 0, 1, |w| w.zeros(8));
                } else {
                    w.full(b"smhd", 0, 0, |w| w.zeros(4));
                }
                w.boxed(b"dinf", |w| {
                    w.full(b"dref", 0, 0, |w| {
                        w.u32(1);
                        w.full(b"url ", 0, 1, |_| {});
                    });
                });
                stbl(w, id, track, long);
            });
        });
    });
}

fn esds(w: &mut Writer, id: u32, object_type: u8, stream_type: u8, bitrate: u32, specific: &[u8]) {
    w.full(b"esds", 0, 0, |w| {
        w.descriptor(0x03, |w| {
            w.u16(id as u16);
            w.u8(0);
            w.descriptor(0x04, |w| {
                w.u8(object_type);
                w.u8(stream_type);
                w.u24(0);
                w.u32(bitrate);
                w.u32(bitrate);
                if !specific.is_empty() {
                    w.descriptor(0x05, |w| w.bytes(specific));
                }
            });
            w.descriptor(0x06, |w| w.u8(0x02));
        });
    });
}

fn sample_entry(w: &mut Writer, id: u32, track: &TrackState) {
    let config = &track.config;
    let seconds = track.duration as f64 / config.timescale as f64;
    let bitrate = if seconds > 0.0 {
        (track.bytes as f64 * 8.0 / seconds) as u32
    } else {
        0
    };
    match (&config.kind, &config.codec) {
        (MuxKind::Video { width, height, .. }, codec) => {
            w.boxed(b"mp4v", |w| {
                w.zeros(6);
                w.u16(1);
                w.zeros(16);
                w.u16(*width as u16);
                w.u16(*height as u16);
                w.u32(0x0048_0000);
                w.u32(0x0048_0000);
                w.zeros(4);
                w.u16(1);
                w.zeros(32);
                w.u16(0x0018);
                w.u16(0xFFFF);
                match codec {
                    MuxCodec::Mpeg2 { header } => esds(w, id, 0x61, 0x11, bitrate, header),
                    _ => esds(w, id, 0x6C, 0x11, bitrate, &[]),
                }
            });
        }
        (
            MuxKind::Audio {
                channels,
                sample_rate,
            },
            codec,
        ) => {
            let (object_type, specific): (u8, &[u8]) = match codec {
                MuxCodec::Aac { config } => (0x40, config),
                _ => (0x6B, &[]),
            };
            w.boxed(b"mp4a", |w| {
                w.zeros(6);
                w.u16(1);
                w.zeros(8);
                w.u16(*channels as u16);
                w.u16(16);
                w.zeros(4);
                w.u32((*sample_rate).min(0xFFFF) << 16);
                esds(w, id, object_type, 0x15, bitrate, specific);
            });
        }
    }
}

fn stbl(w: &mut Writer, id: u32, track: &TrackState, long: bool) {
    w.boxed(b"stbl", |w| {
        w.full(b"stsd", 0, 0, |w| {
            w.u32(1);
            sample_entry(w, id, track);
        });
        w.full(b"stts", 0, 0, |w| {
            w.u32(track.durations.len() as u32);
            for &(count, delta) in &track.durations {
                w.u32(count);
                w.u32(delta);
            }
        });
        if track.offsets.iter().any(|&(_, offset)| offset != 0) {
            w.full(b"ctts", 0, 0, |w| {
                w.u32(track.offsets.len() as u32);
                for &(count, offset) in &track.offsets {
                    w.u32(count);
                    w.u32(offset);
                }
            });
        }
        if !track.unsynced.is_empty() {
            let synced: Vec<u32> = (1..=track.sizes.len() as u32)
                .filter(|n| track.unsynced.binary_search(n).is_err())
                .collect();
            w.full(b"stss", 0, 0, |w| {
                w.u32(synced.len() as u32);
                synced.iter().for_each(|&n| w.u32(n));
            });
        }
        w.full(b"stsc", 0, 0, |w| {
            let mut runs: Vec<(u32, u32)> = Vec::new();
            for (i, &(_, count)) in track.chunks.iter().enumerate() {
                if runs.last().is_none_or(|&(_, c)| c != count) {
                    runs.push((i as u32 + 1, count));
                }
            }
            w.u32(runs.len() as u32);
            for (first, count) in runs {
                w.u32(first);
                w.u32(count);
                w.u32(1);
            }
        });
        w.full(b"stsz", 0, 0, |w| {
            w.u32(0);
            w.u32(track.sizes.len() as u32);
            track.sizes.iter().for_each(|&s| w.u32(s));
        });
        if long {
            w.full(b"co64", 0, 0, |w| {
                w.u32(track.chunks.len() as u32);
                track.chunks.iter().for_each(|&(offset, _)| w.u64(offset));
            });
        } else {
            w.full(b"stco", 0, 0, |w| {
                w.u32(track.chunks.len() as u32);
                track
                    .chunks
                    .iter()
                    .for_each(|&(offset, _)| w.u32(offset as u32));
            });
        }
    });
}
