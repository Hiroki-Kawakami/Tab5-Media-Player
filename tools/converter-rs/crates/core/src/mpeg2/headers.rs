// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use super::bits::{BitWriter, Sink};
use super::{Matrix, Settings};
use crate::framerate::Rate;

const FRAME_RATES: [(u64, u64); 8] = [
    (24000, 1001),
    (24, 1),
    (25, 1),
    (30000, 1001),
    (30, 1),
    (50, 1),
    (60000, 1001),
    (60, 1),
];

pub fn frame_rate_code(rate: Rate) -> (u32, u32, u32) {
    let (num, den) = (rate.num() as u128, rate.den() as u128);
    let mut best = (4, 0, 0, f64::INFINITY);
    for n in 0..4u64 {
        for d in 0..32u64 {
            for (code, &(n0, d0)) in FRAME_RATES.iter().enumerate() {
                let (cn, cd) = (n0 as u128 * (n + 1) as u128, d0 as u128 * (d + 1) as u128);
                if cn * den == num * cd {
                    return (code as u32 + 1, n as u32, d as u32);
                }
                let error = (cn as f64 / cd as f64 - rate.as_f64()).abs();
                if error < best.3 {
                    best = (code as u32 + 1, n as u32, d as u32, error);
                }
            }
        }
    }
    (best.0, best.1, best.2)
}

struct Level {
    code: u32,
    bit_rate: u32,
    vbv: u32,
}

fn level(settings: &Settings) -> Level {
    let fps = settings.rate.as_f64();
    let (w, h) = (settings.width, settings.height);
    if w <= 720 && h <= 576 && fps <= 30.0 {
        Level {
            code: 0x48,
            bit_rate: 15_000_000,
            vbv: 1_835_008,
        }
    } else if w <= 1440 && h <= 1152 && fps <= 60.0 {
        Level {
            code: 0x46,
            bit_rate: 60_000_000,
            vbv: 7_340_032,
        }
    } else {
        Level {
            code: 0x44,
            bit_rate: 80_000_000,
            vbv: 9_781_248,
        }
    }
}

pub fn sequence(settings: &Settings) -> Vec<u8> {
    let mut w = BitWriter::default();
    let (code, ext_n, ext_d) = frame_rate_code(settings.rate);
    let level = level(settings);
    let bit_rate = level.bit_rate / 400;
    let vbv = level.vbv / 16384;
    let (width, height) = (settings.width as u32, settings.height as u32);
    w.start_code(0xB3);
    w.put(width & 0xFFF, 12);
    w.put(height & 0xFFF, 12);
    w.put(1, 4);
    w.put(code, 4);
    w.put(bit_rate & 0x3FFFF, 18);
    w.put(1, 1);
    w.put(vbv & 0x3FF, 10);
    w.put(0, 3);
    w.start_code(0xB5);
    w.put(1, 4);
    w.put(level.code, 8);
    w.put(1, 1);
    w.put(1, 2);
    w.put(width >> 12, 2);
    w.put(height >> 12, 2);
    w.put(bit_rate >> 18, 12);
    w.put(1, 1);
    w.put(vbv >> 10, 8);
    w.put(u32::from(settings.bframes == 0), 1);
    w.put(ext_n, 2);
    w.put(ext_d, 5);
    let Some(matrix) = settings.matrix else {
        return w.finish();
    };
    let colour = match matrix {
        Matrix::Bt709 => 1,
        Matrix::Bt601 => 6,
    };
    w.start_code(0xB5);
    w.put(2, 4);
    w.put(5, 3);
    w.put(1, 1);
    w.put(colour, 8);
    w.put(colour, 8);
    w.put(colour, 8);
    w.put(width, 14);
    w.put(1, 1);
    w.put(height, 14);
    w.finish()
}

pub fn gop(w: &mut BitWriter, first_frame: u64, rate: Rate) {
    let fps = (rate.as_f64().round() as u64).max(1);
    let seconds = first_frame / fps;
    w.start_code(0xB8);
    w.put(0, 1);
    w.put((seconds / 3600 % 24) as u32, 5);
    w.put((seconds / 60 % 60) as u32, 6);
    w.put(1, 1);
    w.put((seconds % 60) as u32, 6);
    w.put((first_frame % fps) as u32, 6);
    w.put(1, 1);
    w.put(0, 1);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rate(num: u64, den: u64) -> Rate {
        Rate::new(num, den).unwrap()
    }

    #[test]
    fn frame_rates() {
        assert_eq!(frame_rate_code(rate(30000, 1001)), (4, 0, 0));
        assert_eq!(frame_rate_code(rate(25, 1)), (3, 0, 0));
        assert_eq!(frame_rate_code(rate(60, 1)), (8, 0, 0));
        let (code, n, d) = frame_rate_code(rate(15, 1));
        let (n0, d0) = FRAME_RATES[code as usize - 1];
        assert_eq!(n0 * (n as u64 + 1), 15 * d0 * (d as u64 + 1));
        let (code, n, d) = frame_rate_code(rate(12, 1));
        let (n0, d0) = FRAME_RATES[code as usize - 1];
        assert_eq!(n0 * (n as u64 + 1), 12 * d0 * (d as u64 + 1));
    }
}
