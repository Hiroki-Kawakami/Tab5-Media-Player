// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod dct;
mod estimate;
mod huffman;
mod tables;

pub use estimate::SizeModel;
use huffman::Table;
use tables::{QuantTables, ZIGZAG};

const MAX_AC: i16 = 1023;
const COEFFICIENT_SCALE: f32 = 8.0;
const MCU_BLOCKS: usize = 6;

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum HuffmanMode {
    Optimal,
    Standard,
}

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub min_quality: u8,
    pub max_frame: usize,
    pub huffman: HuffmanMode,
}

pub struct Frame<'a> {
    pub width: usize,
    pub height: usize,
    pub y: &'a [u8],
    pub cb: &'a [u8],
    pub cr: &'a [u8],
}

impl<'a> Frame<'a> {
    pub fn bytes(width: usize, height: usize) -> usize {
        width * height + 2 * (width / 2) * (height / 2)
    }

    pub fn from_yuv420p(data: &'a [u8], width: usize, height: usize) -> Self {
        let luma = width * height;
        let chroma = (width / 2) * (height / 2);
        Self {
            width,
            height,
            y: &data[..luma],
            cb: &data[luma..luma + chroma],
            cr: &data[luma + chroma..luma + 2 * chroma],
        }
    }
}

pub struct Coefficients {
    width: usize,
    height: usize,
    blocks: Vec<[i16; 64]>,
}

pub struct Encoded {
    pub data: Vec<u8>,
    pub quality: u8,
    pub fits: bool,
}

pub fn analyze(frame: &Frame) -> (Coefficients, SizeModel) {
    let coefficients = transform(frame);
    let model = SizeModel::new(&coefficients.blocks);
    (coefficients, model)
}

pub fn encode(coefficients: &Coefficients, quality: u8, limits: &Limits) -> Encoded {
    let at = |q| encode_blocks(coefficients, q, limits.huffman);
    let first = at(quality);
    if first.len() <= limits.max_frame {
        return Encoded {
            data: first,
            quality,
            fits: true,
        };
    }
    let min_quality = limits.min_quality.min(quality);
    let (mut lo, mut hi) = (min_quality, quality.saturating_sub(1));
    let mut best = None;
    while lo <= hi && hi >= min_quality {
        let mid = lo + (hi - lo) / 2;
        let data = at(mid);
        if data.len() <= limits.max_frame {
            best = Some((data, mid));
            lo = mid + 1;
        } else if mid == 0 {
            break;
        } else {
            hi = mid - 1;
        }
    }
    match best {
        Some((data, quality)) => Encoded {
            data,
            quality,
            fits: true,
        },
        None => {
            let data = if min_quality == quality {
                first
            } else {
                at(min_quality)
            };
            let fits = data.len() <= limits.max_frame;
            Encoded {
                data,
                quality: min_quality,
                fits,
            }
        }
    }
}

fn block_at(plane: &[u8], stride: usize, rows: usize, bx: usize, by: usize) -> [f32; 64] {
    std::array::from_fn(|i| {
        let x = (bx + i % 8).min(stride - 1);
        let y = (by + i / 8).min(rows - 1);
        plane[y * stride + x] as f32 - 128.0
    })
}

fn stored(block: &[f32; 64]) -> [i16; 64] {
    let coefficients = dct::forward(block);
    coefficients.map(|c| (c * COEFFICIENT_SCALE).round() as i16)
}

fn transform(frame: &Frame) -> Coefficients {
    let (w, h) = (frame.width, frame.height);
    let (cw, ch) = (w / 2, h / 2);
    let mcus_x = w.div_ceil(16);
    let mcus_y = h.div_ceil(16);
    let mut blocks = Vec::with_capacity(mcus_x * mcus_y * MCU_BLOCKS);
    for my in 0..mcus_y {
        for mx in 0..mcus_x {
            for (dx, dy) in [(0, 0), (8, 0), (0, 8), (8, 8)] {
                blocks.push(stored(&block_at(frame.y, w, h, mx * 16 + dx, my * 16 + dy)));
            }
            for plane in [frame.cb, frame.cr] {
                blocks.push(stored(&block_at(plane, cw, ch, mx * 8, my * 8)));
            }
        }
    }
    Coefficients {
        width: w,
        height: h,
        blocks,
    }
}

fn quantize(coefficients: &Coefficients, tables: &QuantTables) -> Vec<[i16; 64]> {
    coefficients
        .blocks
        .iter()
        .enumerate()
        .map(|(i, block)| {
            let table = if i % MCU_BLOCKS < 4 {
                &tables.luma
            } else {
                &tables.chroma
            };
            std::array::from_fn(|k| {
                let n = ZIGZAG[k];
                let divisor = table[n] as f32 * COEFFICIENT_SCALE;
                let q = (block[n] as f32 / divisor).round() as i16;
                if k == 0 { q } else { q.clamp(-MAX_AC, MAX_AC) }
            })
        })
        .collect()
}

fn category(value: i32) -> u8 {
    (32 - value.unsigned_abs().leading_zeros()) as u8
}

fn magnitude_bits(value: i32, category: u8) -> u16 {
    let v = if value < 0 { value - 1 } else { value };
    (v as u32 & ((1u32 << category) - 1)) as u16
}

enum Symbol {
    Dc {
        chroma: bool,
        category: u8,
        diff: i32,
    },
    Ac {
        chroma: bool,
        symbol: u8,
        value: i32,
        category: u8,
    },
}

fn symbols(quantized: &[[i16; 64]], mut sink: impl FnMut(Symbol)) {
    let mut predictors = [0i32; 3];
    for (i, block) in quantized.iter().enumerate() {
        let slot = i % MCU_BLOCKS;
        let component = slot.saturating_sub(3);
        let chroma = slot >= 4;
        let dc = block[0] as i32;
        let diff = dc - predictors[component];
        predictors[component] = dc;
        sink(Symbol::Dc {
            chroma,
            category: category(diff),
            diff,
        });

        let mut run = 0u8;
        for &coef in &block[1..] {
            if coef == 0 {
                run += 1;
                continue;
            }
            while run > 15 {
                sink(Symbol::Ac {
                    chroma,
                    symbol: 0xF0,
                    value: 0,
                    category: 0,
                });
                run -= 16;
            }
            let value = coef as i32;
            let cat = category(value);
            sink(Symbol::Ac {
                chroma,
                symbol: (run << 4) | cat,
                value,
                category: cat,
            });
            run = 0;
        }
        if run > 0 {
            sink(Symbol::Ac {
                chroma,
                symbol: 0x00,
                value: 0,
                category: 0,
            });
        }
    }
}

struct Tables {
    dc: [Table; 2],
    ac: [Table; 2],
}

fn build_tables(quantized: &[[i16; 64]], mode: HuffmanMode) -> Tables {
    match mode {
        HuffmanMode::Standard => Tables {
            dc: [
                Table::new(tables::standard_dc_luma()),
                Table::new(tables::standard_dc_chroma()),
            ],
            ac: [
                Table::new(tables::standard_ac_luma()),
                Table::new(tables::standard_ac_chroma()),
            ],
        },
        HuffmanMode::Optimal => {
            let mut dc = [[0u32; 256]; 2];
            let mut ac = [[0u32; 256]; 2];
            symbols(quantized, |s| match s {
                Symbol::Dc {
                    chroma, category, ..
                } => dc[chroma as usize][category as usize] += 1,
                Symbol::Ac { chroma, symbol, .. } => ac[chroma as usize][symbol as usize] += 1,
            });
            Tables {
                dc: dc.map(|f| Table::new(huffman::optimal(&f))),
                ac: ac.map(|f| Table::new(huffman::optimal(&f))),
            }
        }
    }
}

struct BitWriter {
    out: Vec<u8>,
    acc: u64,
    count: u32,
}

impl BitWriter {
    fn put(&mut self, bits: u16, len: u8) {
        if len == 0 {
            return;
        }
        self.acc = (self.acc << len) | bits as u64;
        self.count += len as u32;
        while self.count >= 8 {
            self.count -= 8;
            let byte = (self.acc >> self.count) as u8;
            self.out.push(byte);
            if byte == 0xFF {
                self.out.push(0x00);
            }
        }
        self.acc &= (1u64 << self.count) - 1;
    }

    fn finish(mut self) -> Vec<u8> {
        if self.count > 0 {
            let pad = 8 - self.count as u8;
            self.put((1u16 << pad) - 1, pad);
        }
        self.out
    }
}

fn segment(out: &mut Vec<u8>, marker: u8, payload: &[u8]) {
    out.extend([0xFF, marker]);
    out.extend(((payload.len() + 2) as u16).to_be_bytes());
    out.extend(payload);
}

fn encode_blocks(coefficients: &Coefficients, quality: u8, mode: HuffmanMode) -> Vec<u8> {
    let quant = QuantTables::for_quality(quality);
    let quantized = quantize(coefficients, &quant);
    let tables = build_tables(&quantized, mode);

    let mut out = vec![0xFF, 0xD8];
    let mut dqt = Vec::with_capacity(130);
    for (id, table) in [(0u8, &quant.luma), (1, &quant.chroma)] {
        dqt.push(id);
        dqt.extend(ZIGZAG.map(|n| table[n]));
    }
    segment(&mut out, 0xDB, &dqt);

    let mut sof = vec![8];
    sof.extend((coefficients.height as u16).to_be_bytes());
    sof.extend((coefficients.width as u16).to_be_bytes());
    sof.extend([3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1]);
    segment(&mut out, 0xC0, &sof);

    let mut dht = Vec::new();
    for (class, set) in [(0u8, &tables.dc), (1, &tables.ac)] {
        for (id, table) in set.iter().enumerate() {
            dht.push(class << 4 | id as u8);
            dht.extend(table.spec.bits);
            dht.extend(&table.spec.values);
        }
    }
    segment(&mut out, 0xC4, &dht);
    segment(&mut out, 0xDA, &[3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0]);

    let mut writer = BitWriter {
        out,
        acc: 0,
        count: 0,
    };
    symbols(&quantized, |s| match s {
        Symbol::Dc {
            chroma,
            category,
            diff,
        } => {
            let code = tables.dc[chroma as usize].codes[category as usize];
            writer.put(code.bits, code.len);
            writer.put(magnitude_bits(diff, category), category);
        }
        Symbol::Ac {
            chroma,
            symbol,
            value,
            category,
        } => {
            let code = tables.ac[chroma as usize].codes[symbol as usize];
            writer.put(code.bits, code.len);
            writer.put(magnitude_bits(value, category), category);
        }
    });
    let mut out = writer.finish();
    out.extend([0xFF, 0xD9]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame_data(width: usize, height: usize) -> Vec<u8> {
        let mut data = vec![0u8; Frame::bytes(width, height)];
        let (luma, chroma) = data.split_at_mut(width * height);
        for (i, px) in luma.iter_mut().enumerate() {
            let (x, y) = (i % width, i / width);
            *px = ((x * 7 + y * 13) ^ (x * y)) as u8;
        }
        for (i, px) in chroma.iter_mut().enumerate() {
            *px = (i * 31 % 251) as u8;
        }
        data
    }

    struct Settings {
        quality: u8,
        limits: Limits,
    }

    fn settings(quality: u8, min_quality: u8, max_frame: usize, huffman: HuffmanMode) -> Settings {
        Settings {
            quality,
            limits: Limits {
                min_quality,
                max_frame,
                huffman,
            },
        }
    }

    fn encode_at(width: usize, height: usize, s: Settings) -> Encoded {
        let data = frame_data(width, height);
        let (coefficients, _) = analyze(&Frame::from_yuv420p(&data, width, height));
        encode(&coefficients, s.quality, &s.limits)
    }

    #[test]
    fn estimate_tracks_actual_size() {
        let data = frame_data(256, 128);
        let (coefficients, model) = analyze(&Frame::from_yuv420p(&data, 256, 128));
        let limits = Limits {
            min_quality: 1,
            max_frame: usize::MAX,
            huffman: HuffmanMode::Optimal,
        };
        let ratios: Vec<f32> = [30, 50, 70, 80, 90]
            .iter()
            .map(|&q| encode(&coefficients, q, &limits).data.len() as f32 / model.bytes(q))
            .collect();
        let (lo, hi) = ratios
            .iter()
            .fold((f32::MAX, 0f32), |(lo, hi), &r| (lo.min(r), hi.max(r)));
        assert!(hi / lo < 1.5, "{ratios:?}");
    }

    #[test]
    fn writes_a_baseline_jpeg() {
        for huffman in [HuffmanMode::Optimal, HuffmanMode::Standard] {
            let e = encode_at(50, 30, settings(80, 30, usize::MAX, huffman));
            assert_eq!(&e.data[..2], &[0xFF, 0xD8]);
            assert_eq!(&e.data[e.data.len() - 2..], &[0xFF, 0xD9]);
            let sof = e.data.windows(2).position(|w| w == [0xFF, 0xC0]).unwrap();
            assert_eq!(&e.data[sof + 5..sof + 9], &[0, 30, 0, 50]);
            assert_eq!(e.quality, 80);
            assert!(e.fits);
        }
    }

    #[test]
    fn optimal_tables_are_smaller() {
        let optimal = encode_at(256, 128, settings(80, 30, usize::MAX, HuffmanMode::Optimal));
        let standard = encode_at(
            256,
            128,
            settings(80, 30, usize::MAX, HuffmanMode::Standard),
        );
        assert!(optimal.data.len() < standard.data.len());
    }

    #[test]
    fn lowers_quality_to_fit_max_frame() {
        let full = encode_at(256, 128, settings(90, 10, usize::MAX, HuffmanMode::Optimal));
        let limit = full.data.len() / 2;
        let fitted = encode_at(256, 128, settings(90, 10, limit, HuffmanMode::Optimal));
        assert!(fitted.fits);
        assert!(fitted.data.len() <= limit);
        assert!(fitted.quality < 90 && fitted.quality >= 10);
        let one_up = encode_at(
            256,
            128,
            settings(
                fitted.quality + 1,
                fitted.quality + 1,
                usize::MAX,
                HuffmanMode::Optimal,
            ),
        );
        assert!(one_up.data.len() > limit);
    }

    #[test]
    fn reports_frames_that_cannot_fit() {
        let e = encode_at(256, 128, settings(80, 40, 100, HuffmanMode::Optimal));
        assert!(!e.fits);
        assert_eq!(e.quality, 40);
    }

    #[test]
    fn category_and_magnitude_bits() {
        assert_eq!(category(0), 0);
        assert_eq!(category(1), 1);
        assert_eq!(category(-1), 1);
        assert_eq!(category(1023), 10);
        assert_eq!(magnitude_bits(-1, 1), 0);
        assert_eq!(magnitude_bits(-3, 2), 0);
        assert_eq!(magnitude_bits(3, 2), 3);
    }
}
