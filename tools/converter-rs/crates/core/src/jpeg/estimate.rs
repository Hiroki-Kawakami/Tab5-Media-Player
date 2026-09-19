// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use super::tables::{CHROMA_QUANT, LUMA_QUANT, scale};
use super::{COEFFICIENT_SCALE, MCU_BLOCKS};

const BINS_PER_OCTAVE: f32 = 8.0;
const MIN_LOG2: f32 = -10.0;
const BINS: usize = 22 * BINS_PER_OCTAVE as usize;
const AC_CODE_BITS: f32 = 4.15;
const MAGNITUDE_WEIGHT: f32 = 1.06;
const BLOCK_BITS: f32 = 2.82;
const HEADER_BYTES: f32 = 600.0;
const STUFFING: f32 = 1.0 + 1.0 / 256.0;

pub struct SizeModel {
    ac: [[u32; BINS]; 2],
    dc: [[u32; BINS]; 2],
    blocks: [u32; 2],
}

fn bin(normalized: f32) -> usize {
    let position = (normalized.log2() - MIN_LOG2) * BINS_PER_OCTAVE;
    (position.max(0.0) as usize).min(BINS - 1)
}

fn representative(bin: usize) -> f32 {
    (MIN_LOG2 + (bin as f32 + 0.5) / BINS_PER_OCTAVE).exp2()
}

fn magnitude_bits(quantized: f32) -> f32 {
    (quantized.round().log2().floor() + 1.0).max(1.0)
}

impl SizeModel {
    pub fn new(blocks: &[[i16; 64]]) -> Self {
        let mut model = Self {
            ac: [[0; BINS]; 2],
            dc: [[0; BINS]; 2],
            blocks: [0; 2],
        };
        let mut predictors = [0f32; 3];
        for (i, block) in blocks.iter().enumerate() {
            let slot = i % MCU_BLOCKS;
            let chroma = slot >= 4;
            let class = chroma as usize;
            let base = if chroma { &CHROMA_QUANT } else { &LUMA_QUANT };
            model.blocks[class] += 1;

            let component = slot.saturating_sub(3);
            let dc = block[0] as f32 / COEFFICIENT_SCALE / base[0] as f32;
            let diff = (dc - predictors[component]).abs();
            predictors[component] = dc;
            if diff > 0.0 {
                model.dc[class][bin(diff)] += 1;
            }
            for (n, &coefficient) in block.iter().enumerate().skip(1) {
                if coefficient != 0 {
                    let normalized =
                        coefficient.unsigned_abs() as f32 / COEFFICIENT_SCALE / base[n] as f32;
                    model.ac[class][bin(normalized)] += 1;
                }
            }
        }
        model
    }

    pub fn bytes(&self, quality: u8) -> f32 {
        let factor = 100.0 / scale(quality).max(1) as f32;
        let mut bits = 0.0;
        for class in 0..2 {
            bits += self.blocks[class] as f32 * BLOCK_BITS;
            for b in 0..BINS {
                let (ac, dc) = (self.ac[class][b], self.dc[class][b]);
                if ac == 0 && dc == 0 {
                    continue;
                }
                let quantized = representative(b) * factor;
                if quantized < 0.5 {
                    continue;
                }
                let magnitude = magnitude_bits(quantized) * MAGNITUDE_WEIGHT;
                bits += ac as f32 * (AC_CODE_BITS + magnitude) + dc as f32 * magnitude;
            }
        }
        bits / 8.0 * STUFFING + HEADER_BYTES
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bins_round_trip() {
        for value in [0.01f32, 0.5, 1.0, 3.7, 100.0] {
            let r = representative(bin(value));
            assert!(
                (r / value).log2().abs() <= 1.0 / BINS_PER_OCTAVE,
                "{value} -> {r}"
            );
        }
    }

    #[test]
    fn estimate_falls_with_quality() {
        let blocks: Vec<[i16; 64]> = (0..600)
            .map(|i| {
                std::array::from_fn(|n| {
                    (((i * 31 + n * 17) % 97) as i16 - 48) * 40 / (n as i16 + 1)
                })
            })
            .collect();
        let model = SizeModel::new(&blocks);
        let mut last = f32::MAX;
        for q in [95, 80, 60, 40, 20] {
            let bytes = model.bytes(q);
            assert!(bytes < last, "q{q}: {bytes} >= {last}");
            last = bytes;
        }
    }
}
