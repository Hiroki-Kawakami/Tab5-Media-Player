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

#[derive(Clone, Debug, PartialEq)]
pub struct SizeModel {
    ac: [[u32; BINS]; 2],
    dc: [[u32; BINS]; 2],
    blocks: [u32; 2],
}

const STEPS: [u32; 7] = [
    0x0B_95C2, 0x18_37F1, 0x25_FED7, 0x35_04F4, 0x45_672B, 0x57_44FD, 0x6A_C0C7,
];

fn bin(normalized: f32) -> usize {
    let bits = normalized.to_bits();
    let octave = (bits >> 23) as i32 - 127 - MIN_LOG2 as i32;
    let mantissa = bits & 0x7F_FFFF;
    let step: i32 = STEPS.iter().map(|&s| i32::from(mantissa >= s)).sum();
    (octave * BINS_PER_OCTAVE as i32 + step).clamp(0, BINS as i32 - 1) as usize
}

fn representative(bin: usize) -> f32 {
    (MIN_LOG2 + (bin as f32 + 0.5) / BINS_PER_OCTAVE).exp2()
}

fn magnitude_bits(quantized: f32) -> f32 {
    (quantized.round().log2().floor() + 1.0).max(1.0)
}

impl SizeModel {
    pub const WORDS: usize = 4 * BINS + 2;

    pub fn to_words(&self) -> Vec<u32> {
        let mut words = Vec::with_capacity(Self::WORDS);
        for class in 0..2 {
            words.extend(self.ac[class]);
            words.extend(self.dc[class]);
        }
        words.extend(self.blocks);
        words
    }

    pub fn from_words(words: &[u32]) -> Option<Self> {
        if words.len() != Self::WORDS {
            return None;
        }
        let mut model = Self {
            ac: [[0; BINS]; 2],
            dc: [[0; BINS]; 2],
            blocks: [words[4 * BINS], words[4 * BINS + 1]],
        };
        for class in 0..2 {
            let base = class * 2 * BINS;
            model.ac[class].copy_from_slice(&words[base..base + BINS]);
            model.dc[class].copy_from_slice(&words[base + BINS..base + 2 * BINS]);
        }
        Some(model)
    }

    pub fn new(blocks: &[[i16; 64]]) -> Self {
        let mut model = Self {
            ac: [[0; BINS]; 2],
            dc: [[0; BINS]; 2],
            blocks: [0; 2],
        };
        let inverse =
            [&LUMA_QUANT, &CHROMA_QUANT].map(|b| b.map(|q| 1.0 / (COEFFICIENT_SCALE * q as f32)));
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
            #[cfg(all(target_arch = "wasm32", target_feature = "simd128"))]
            {
                let bins = wasm::bins(block, &inverse[class]);
                let mut mask = super::nonzero_ac(block);
                while mask != 0 {
                    let n = mask.trailing_zeros() as usize;
                    mask &= mask - 1;
                    model.ac[class][bins[n] as usize] += 1;
                }
            }
            #[cfg(not(all(target_arch = "wasm32", target_feature = "simd128")))]
            for (n, &coefficient) in block.iter().enumerate().skip(1) {
                if coefficient != 0 {
                    let normalized = coefficient.unsigned_abs() as f32 * inverse[class][n];
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

#[cfg(all(target_arch = "wasm32", target_feature = "simd128"))]
mod wasm {
    use core::arch::wasm32::*;

    use super::{BINS, BINS_PER_OCTAVE, MIN_LOG2, STEPS};

    fn position(v: v128, inverse: *const v128) -> v128 {
        let steps = STEPS.map(|s| u32x4_splat(s));
        let normalized = unsafe { f32x4_mul(f32x4_convert_u32x4(v), v128_load(inverse)) };
        let octave = i32x4_sub(
            u32x4_shr(normalized, 23),
            i32x4_splat(127 + MIN_LOG2 as i32),
        );
        let mantissa = v128_and(normalized, u32x4_splat(0x7F_FFFF));
        let mut position = i32x4_mul(octave, i32x4_splat(BINS_PER_OCTAVE as i32));
        for s in steps {
            position = i32x4_sub(position, u32x4_ge(mantissa, s));
        }
        i32x4_min(
            i32x4_max(position, i32x4_splat(0)),
            i32x4_splat(BINS as i32 - 1),
        )
    }

    pub fn bins(block: &[i16; 64], inverse: &[f32; 64]) -> [i32; 64] {
        let mut out = [0i32; 64];
        let p = block.as_ptr() as *const v128;
        let w = inverse.as_ptr() as *const v128;
        let q = out.as_mut_ptr() as *mut v128;
        for i in 0..8 {
            unsafe {
                let abs = i16x8_abs(v128_load(p.add(i)));
                v128_store(
                    q.add(2 * i),
                    position(u32x4_extend_low_u16x8(abs), w.add(2 * i)),
                );
                v128_store(
                    q.add(2 * i + 1),
                    position(u32x4_extend_high_u16x8(abs), w.add(2 * i + 1)),
                );
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bins_follow_log2() {
        let mut state = 7u32;
        for _ in 0..100_000 {
            state = state.wrapping_mul(1_103_515_245).wrapping_add(12345);
            let value = (state >> 8) as f32 / 4096.0 + 1e-3;
            let exact = ((value as f64).log2() - MIN_LOG2 as f64) * BINS_PER_OCTAVE as f64;
            let expected = (exact.floor().max(0.0) as usize).min(BINS - 1);
            assert_eq!(bin(value), expected, "{value}");
        }
    }

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
