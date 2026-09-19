// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::{BTreeMap, HashMap, VecDeque};

use anyhow::{Result, bail};
use tab5conv_core::color::{self, Matrix};
use tab5conv_core::container::interleave::{Interleaver, Sample};
use tab5conv_core::framerate::Rate;
use tab5conv_core::jpeg::Frame;
use tab5conv_core::mpeg2::{self, GopEncoder, PictureType, Settings};
use tab5conv_core::video::Picture;
use tab5conv_core::video::mpeg2::{Params, Rate as RateMode};
use tab5conv_core::yuv::{Converter, Geometry, Layout, Output, Source, SourceColor};
use wasm_bindgen::prelude::*;

use crate::js_error;

pub struct Config {
    pub settings: Settings,
    pub keyint: u32,
}

impl Config {
    pub fn new(
        width: u32,
        height: u32,
        rate: Rate,
        params: &Params,
        matrix: Option<Matrix>,
    ) -> Result<Self> {
        let RateMode::Qscale(qscale) = params.rate else {
            bail!("mpeg2: bitrate= is not available in the browser; use qscale");
        };
        if !params.closed_gop {
            bail!(
                "mpeg2: gop=open is not available in the browser, where every GOP is encoded on its own"
            );
        }
        Ok(Self {
            settings: Settings {
                width: width as usize,
                height: height as usize,
                rate,
                qscale: qscale as u8,
                bframes: params.bframes as usize,
                hq: params.hq,
                matrix,
            },
            keyint: params.keyint,
        })
    }

    pub fn to_words(&self) -> Vec<f64> {
        let s = &self.settings;
        vec![
            s.width as f64,
            s.height as f64,
            s.rate.num() as f64,
            s.rate.den() as f64,
            s.qscale as f64,
            s.bframes as f64,
            f64::from(u8::from(s.hq)),
            matrix_code(s.matrix),
            self.keyint as f64,
        ]
    }

    fn from_words(words: &[f64]) -> Result<Self> {
        let [width, height, num, den, qscale, bframes, hq, matrix, keyint] = words[..] else {
            bail!("bad MPEG-2 configuration");
        };
        let Some(rate) = Rate::new(num as u64, den as u64) else {
            bail!("bad MPEG-2 frame rate");
        };
        Ok(Self {
            settings: Settings {
                width: width as usize,
                height: height as usize,
                rate,
                qscale: qscale as u8,
                bframes: bframes as usize,
                hq: hq != 0.0,
                matrix: matrix_from_code(matrix),
            },
            keyint: keyint as u32,
        })
    }
}

pub fn matrix_code(matrix: Option<Matrix>) -> f64 {
    match matrix {
        Some(Matrix::Bt709) => 1.0,
        Some(Matrix::Bt601) => 6.0,
        None => 2.0,
    }
}

pub fn matrix_from_code(code: f64) -> Option<Matrix> {
    match code as u8 {
        1 => Some(Matrix::Bt709),
        5 | 6 => Some(Matrix::Bt601),
        _ => None,
    }
}

#[wasm_bindgen]
pub struct YuvScaler {
    converter: Converter,
    planes: Vec<(usize, usize)>,
}

#[wasm_bindgen]
impl YuvScaler {
    #[wasm_bindgen(constructor)]
    pub fn new(geometry: Vec<f64>) -> Result<YuvScaler, JsError> {
        let [
            sw,
            sh,
            cw,
            ch,
            source_rotation,
            output_rotation,
            width,
            height,
            output,
        ] = geometry[..]
        else {
            return Err(JsError::new("bad YUV geometry"));
        };
        Ok(Self {
            converter: Converter::new(Geometry {
                scaled: (sw as usize, sh as usize),
                crop: (cw as usize, ch as usize),
                source_rotation: source_rotation as u32,
                output_rotation: output_rotation as u32,
                stored: (width as usize, height as usize),
                output: if output != 0.0 {
                    Output::Bt601Full
                } else {
                    Output::Limited
                },
            }),
            planes: Vec::new(),
        })
    }

    pub fn supports(format: &str) -> bool {
        Layout::parse(format).is_some()
    }

    pub fn convert(
        &mut self,
        format: &str,
        data: &[u8],
        layout: &[u32],
        color: &[u8],
    ) -> Result<Vec<u8>, JsError> {
        let Some(layout_kind) = Layout::parse(format) else {
            return Err(JsError::new(&format!("unsupported frame format {format}")));
        };
        let (&[width, height], planes) = layout.split_at(2.min(layout.len())) else {
            return Err(JsError::new("frame layout needs a width and a height"));
        };
        let &[matrix, full_range] = color else {
            return Err(JsError::new("frame colour needs a matrix and a range"));
        };
        self.planes.clear();
        self.planes.extend(
            planes
                .chunks_exact(2)
                .map(|p| (p[0] as usize, p[1] as usize)),
        );
        let source = Source {
            layout: layout_kind,
            width: width as usize,
            height: height as usize,
            data,
            planes: &self.planes,
        };
        let color = SourceColor {
            matrix: matrix_from_code(matrix as f64).unwrap_or(Matrix::Bt601),
            full_range: full_range != 0,
        };
        self.converter.convert(&source, color).map_err(js_error)
    }
}

pub fn yuv_geometry(picture: &Picture, source_rotation: u32, output: Output) -> Vec<f64> {
    let r = &picture.resize;
    vec![
        r.scaled_width as f64,
        r.scaled_height as f64,
        r.width as f64,
        r.height as f64,
        source_rotation as f64,
        picture.rotation.map_or(0, |r| r.degrees) as f64,
        picture.width as f64,
        picture.height as f64,
        f64::from(u8::from(output == Output::Bt601Full)),
    ]
}

#[wasm_bindgen(js_name = rgbaToYuv420)]
pub fn rgba_to_yuv420(rgba: &[u8], width: usize, height: usize, bt709: bool) -> Vec<u8> {
    let mut out = vec![0; Frame::bytes(width, height)];
    let matrix = if bt709 { Matrix::Bt709 } else { Matrix::Bt601 };
    color::rgba_to_yuv420_limited(rgba, width, height, matrix, &mut out);
    out
}

#[wasm_bindgen]
#[derive(Default)]
pub struct EncodedPictures {
    data: Vec<u8>,
    sizes: Vec<u32>,
    indexes: Vec<u32>,
    kinds: Vec<u8>,
}

#[wasm_bindgen]
impl EncodedPictures {
    #[wasm_bindgen(getter)]
    pub fn data(&self) -> Vec<u8> {
        self.data.clone()
    }

    #[wasm_bindgen(getter)]
    pub fn sizes(&self) -> Vec<u32> {
        self.sizes.clone()
    }

    #[wasm_bindgen(getter)]
    pub fn indexes(&self) -> Vec<u32> {
        self.indexes.clone()
    }

    #[wasm_bindgen(getter)]
    pub fn kinds(&self) -> Vec<u8> {
        self.kinds.clone()
    }
}

impl From<Vec<mpeg2::Picture>> for EncodedPictures {
    fn from(pictures: Vec<mpeg2::Picture>) -> Self {
        let mut out = Self::default();
        for p in pictures {
            out.sizes.push(p.data.len() as u32);
            out.indexes.push(p.index as u32);
            out.kinds.push(p.kind as u8);
            out.data.extend(p.data);
        }
        out
    }
}

#[wasm_bindgen]
pub struct Mpeg2Worker {
    config: Config,
    gops: HashMap<u32, GopEncoder>,
}

#[wasm_bindgen]
impl Mpeg2Worker {
    #[wasm_bindgen(constructor)]
    pub fn new(config: Vec<f64>) -> Result<Mpeg2Worker, JsError> {
        Ok(Self {
            config: Config::from_words(&config).map_err(js_error)?,
            gops: HashMap::new(),
        })
    }

    pub fn push(&mut self, gop: u32, yuv: &[u8]) -> Result<EncodedPictures, JsError> {
        let settings = &self.config.settings;
        if yuv.len() != Frame::bytes(settings.width, settings.height) {
            return Err(JsError::new("mpeg2: frame has the wrong size"));
        }
        let first = gop as u64 * self.config.keyint as u64;
        let encoder = self
            .gops
            .entry(gop)
            .or_insert_with(|| GopEncoder::new(settings, first));
        let frame = Frame::from_yuv420p(yuv, settings.width, settings.height);
        Ok(encoder.push(&frame).map_err(js_error)?.into())
    }

    pub fn finish(&mut self, gop: u32) -> EncodedPictures {
        self.gops
            .remove(&gop)
            .map(|e| e.finish().into())
            .unwrap_or_default()
    }
}

pub struct Pictures<'a> {
    pub data: &'a [u8],
    pub sizes: &'a [u32],
    pub indexes: &'a [u32],
    pub kinds: &'a [u8],
}

struct Pending {
    pictures: VecDeque<(u32, u8, Vec<u8>)>,
    finished: bool,
}

pub struct Mpeg2Mux {
    keyint: u64,
    delay: u64,
    timescale: u32,
    duration: u32,
    gops: BTreeMap<u32, Pending>,
    next_gop: u32,
    coded: u64,
    bytes: [u64; 3],
    counts: [u64; 3],
}

impl Mpeg2Mux {
    pub fn new(config: &Config) -> Self {
        let rate = config.settings.rate;
        let timescale = mpeg2::timescale(rate);
        Self {
            keyint: config.keyint as u64,
            delay: mpeg2::reorder_delay(config.settings.bframes),
            timescale,
            duration: (rate.den() * timescale as u64 / rate.num()) as u32,
            gops: BTreeMap::new(),
            next_gop: 0,
            coded: 0,
            bytes: [0; 3],
            counts: [0; 3],
        }
    }

    pub fn timescale(&self) -> u32 {
        self.timescale
    }

    pub fn skip(&self) -> u32 {
        self.delay as u32 * self.duration
    }

    pub fn written(&self) -> u64 {
        self.coded
    }

    pub fn add(
        &mut self,
        gop: u32,
        pictures: Pictures,
        finished: bool,
        interleaver: &mut Interleaver,
    ) -> Result<()> {
        if gop < self.next_gop {
            bail!("mpeg2: GOP {gop} arrived after it was written");
        }
        let pending = self.gops.entry(gop).or_insert(Pending {
            pictures: VecDeque::new(),
            finished: false,
        });
        let mut at = 0;
        let Pictures {
            data,
            sizes,
            indexes,
            kinds,
        } = pictures;
        for ((&size, &index), &kind) in sizes.iter().zip(indexes).zip(kinds) {
            let end = at + size as usize;
            let Some(bytes) = data.get(at..end) else {
                bail!("mpeg2: picture data is shorter than its sizes");
            };
            pending.pictures.push_back((index, kind, bytes.to_vec()));
            at = end;
        }
        pending.finished |= finished;
        self.drain(interleaver)
    }

    fn drain(&mut self, interleaver: &mut Interleaver) -> Result<()> {
        while let Some(pending) = self.gops.get_mut(&self.next_gop) {
            while let Some((index, kind, data)) = pending.pictures.pop_front() {
                let shown = self.next_gop as u64 * self.keyint + index as u64;
                let Some(offset) = (shown + self.delay).checked_sub(self.coded) else {
                    bail!("mpeg2: picture {shown} is reordered further than the muxer allows");
                };
                let dts_ticks = (self.coded as i64 - self.delay as i64) * self.duration as i64;
                let slot = (kind as usize).clamp(1, 3) - 1;
                self.bytes[slot] += data.len() as u64;
                self.counts[slot] += 1;
                interleaver.push(
                    crate::VIDEO_TRACK,
                    Sample {
                        time: (dts_ticks as i128 * 1_000_000 / self.timescale as i128) as i64,
                        data,
                        duration: self.duration,
                        key: kind == PictureType::I as u8,
                        offset: offset as u32 * self.duration,
                    },
                );
                self.coded += 1;
            }
            if !pending.finished {
                break;
            }
            self.gops.remove(&self.next_gop);
            self.next_gop += 1;
        }
        Ok(())
    }

    pub fn is_idle(&self) -> bool {
        self.gops.is_empty()
    }

    pub fn report(&self, fps: f64) -> Vec<String> {
        let frames: u64 = self.counts.iter().sum();
        if frames == 0 {
            return vec!["mpeg2: no frames".into()];
        }
        let total: u64 = self.bytes.iter().sum();
        let seconds = frames as f64 / fps;
        let average = |slot: usize| {
            if self.counts[slot] == 0 {
                0.0
            } else {
                self.bytes[slot] as f64 / self.counts[slot] as f64 / 1024.0
            }
        };
        vec![
            format!(
                "mpeg2: {frames} frames, {:.1} KiB average, {:.2} Mbit/s average",
                total as f64 / frames as f64 / 1024.0,
                total as f64 * 8.0 / seconds / 1e6
            ),
            format!(
                "mpeg2: I {} ({:.1} KiB), P {} ({:.1} KiB), B {} ({:.1} KiB)",
                self.counts[0],
                average(0),
                self.counts[1],
                average(1),
                self.counts[2],
                average(2)
            ),
        ]
    }
}
