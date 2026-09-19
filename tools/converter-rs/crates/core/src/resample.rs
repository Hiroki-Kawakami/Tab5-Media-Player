// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

const WEIGHT_BITS: u32 = 14;
const EXTRA_BITS: u32 = 4;
const B: f64 = 0.0;
const C: f64 = 0.6;

fn bicubic(x: f64) -> f64 {
    let x = x.abs();
    let v = if x < 1.0 {
        (12.0 - 9.0 * B - 6.0 * C) * x * x * x
            + (-18.0 + 12.0 * B + 6.0 * C) * x * x
            + (6.0 - 2.0 * B)
    } else if x < 2.0 {
        (-B - 6.0 * C) * x * x * x
            + (6.0 * B + 30.0 * C) * x * x
            + (-12.0 * B - 48.0 * C) * x
            + (8.0 * B + 24.0 * C)
    } else {
        0.0
    };
    v / 6.0
}

pub struct Taps {
    pub len: usize,
    pub start: Vec<u32>,
    pub weights: Vec<i16>,
}

impl Taps {
    pub fn new(src: usize, dst: usize) -> Self {
        let scale = src as f64 / dst as f64;
        let stretch = scale.max(1.0);
        let len = ((4.0 * stretch).ceil() as usize + 1).min(src);
        let mut start = Vec::with_capacity(dst);
        let mut weights = Vec::with_capacity(dst * len);
        for d in 0..dst {
            let centre = (d as f64 + 0.5) * scale - 0.5;
            let first = ((centre - 2.0 * stretch).floor() as isize + 1)
                .clamp(0, (src - len) as isize) as usize;
            let mut w = vec![0f64; len];
            let lo = (centre - 2.0 * stretch).floor() as isize;
            let hi = (centre + 2.0 * stretch).ceil() as isize;
            for s in lo..=hi {
                let k = bicubic((s as f64 - centre) / stretch);
                if k != 0.0 {
                    let at = s.clamp(0, src as isize - 1) as usize;
                    let slot = at.clamp(first, first + len - 1) - first;
                    w[slot] += k;
                }
            }
            let sum: f64 = w.iter().sum();
            let one = 1i32 << WEIGHT_BITS;
            let mut fixed: Vec<i32> = w
                .iter()
                .map(|v| (v / sum * one as f64).round() as i32)
                .collect();
            let error = one - fixed.iter().sum::<i32>();
            let peak = (0..len)
                .max_by(|&a, &b| w[a].total_cmp(&w[b]))
                .expect("at least one tap");
            fixed[peak] += error;
            start.push(first as u32);
            weights.extend(fixed.iter().map(|&v| v as i16));
        }
        Self {
            len,
            start,
            weights,
        }
    }
}

fn run<const STRIDE: usize, const CH: usize>(
    h: &Taps,
    v: &Taps,
    rows: &mut Vec<i16>,
    src: &[u8],
    (src_stride, sh): (usize, usize),
    (dw, dh): (usize, usize),
    dst: &mut [u8],
) {
    rows.resize(sh * dw * STRIDE, 0);
    let round = 1i32 << (WEIGHT_BITS - EXTRA_BITS - 1);
    for (y, out) in rows.chunks_exact_mut(dw * STRIDE).enumerate() {
        let row = &src[y * src_stride..];
        for x in 0..dw {
            let first = h.start[x] as usize;
            let weights = &h.weights[x * h.len..(x + 1) * h.len];
            let mut acc = [0i32; CH];
            for (t, &w) in weights.iter().enumerate() {
                let p = &row[(first + t) * STRIDE..];
                for c in 0..CH {
                    acc[c] += p[c] as i32 * w as i32;
                }
            }
            for c in 0..CH {
                out[x * STRIDE + c] = ((acc[c] + round) >> (WEIGHT_BITS - EXTRA_BITS)) as i16;
            }
        }
    }
    let shift = WEIGHT_BITS + EXTRA_BITS;
    let round = 1i32 << (shift - 1);
    for y in 0..dh {
        let first = v.start[y] as usize;
        let weights = &v.weights[y * v.len..(y + 1) * v.len];
        let out = &mut dst[y * dw * STRIDE..(y + 1) * dw * STRIDE];
        for x in 0..dw {
            let mut acc = [0i32; CH];
            for (t, &w) in weights.iter().enumerate() {
                let p = &rows[((first + t) * dw + x) * STRIDE..];
                for c in 0..CH {
                    acc[c] += p[c] as i32 * w as i32;
                }
            }
            for c in 0..CH {
                out[x * STRIDE + c] = ((acc[c] + round) >> shift).clamp(0, 255) as u8;
            }
        }
    }
}

pub struct Resampler {
    pub src: (usize, usize),
    pub dst: (usize, usize),
    pub horizontal: Taps,
    pub vertical: Taps,
    rows: Vec<i16>,
}

impl Resampler {
    pub fn new(src_width: usize, src_height: usize, dst_width: usize, dst_height: usize) -> Self {
        Self {
            src: (src_width, src_height),
            dst: (dst_width, dst_height),
            horizontal: Taps::new(src_width, dst_width),
            vertical: Taps::new(src_height, dst_height),
            rows: Vec::new(),
        }
    }

    pub fn rgba(&mut self, src: &[u8], dst: &mut [u8]) {
        let (sw, sh) = self.src;
        let (dw, dh) = self.dst;
        assert_eq!(src.len(), sw * sh * 4);
        assert_eq!(dst.len(), dw * dh * 4);
        run::<4, 3>(
            &self.horizontal,
            &self.vertical,
            &mut self.rows,
            src,
            (sw * 4, sh),
            self.dst,
            dst,
        );
        for p in dst.chunks_exact_mut(4) {
            p[3] = 255;
        }
    }

    pub fn plane(&mut self, src: &[u8], stride: usize, dst: &mut [u8]) {
        let (sw, sh) = self.src;
        let (dw, dh) = self.dst;
        assert!(stride >= sw && src.len() >= (sh - 1) * stride + sw);
        assert_eq!(dst.len(), dw * dh);
        run::<1, 1>(
            &self.horizontal,
            &self.vertical,
            &mut self.rows,
            src,
            (stride, sh),
            self.dst,
            dst,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image(w: usize, h: usize, f: impl Fn(usize, usize) -> u8) -> Vec<u8> {
        let mut out = Vec::with_capacity(w * h * 4);
        for y in 0..h {
            for x in 0..w {
                let v = f(x, y);
                out.extend([v, v, v, 255]);
            }
        }
        out
    }

    #[test]
    fn weights_sum_to_one() {
        for (src, dst) in [(1920, 640), (1080, 360), (1920, 853), (100, 7), (5, 2)] {
            let taps = Taps::new(src, dst);
            for x in 0..dst {
                let sum: i32 = taps.weights[x * taps.len..(x + 1) * taps.len]
                    .iter()
                    .map(|&w| w as i32)
                    .sum();
                assert_eq!(sum, 1 << WEIGHT_BITS);
                assert!(taps.start[x] as usize + taps.len <= src);
            }
        }
    }

    #[test]
    fn flat_stays_flat() {
        let mut r = Resampler::new(90, 60, 30, 20);
        let mut out = vec![0; 30 * 20 * 4];
        r.rgba(&image(90, 60, |_, _| 77), &mut out);
        assert!(out.chunks(4).all(|p| p == [77, 77, 77, 255]));
    }

    #[test]
    fn thin_lines_are_averaged_not_dropped() {
        let (w, h) = (1920, 1080);
        let src = image(w, h, |x, y| if (x + y) % 3 == 0 { 255 } else { 0 });
        let mut r = Resampler::new(w, h, 640, 360);
        let mut out = vec![0; 640 * 360 * 4];
        r.rgba(&src, &mut out);
        let mean = out.chunks(4).map(|p| p[0] as f64).sum::<f64>() / (640.0 * 360.0);
        assert!((mean - 85.0).abs() < 2.0, "{mean}");
    }
}
