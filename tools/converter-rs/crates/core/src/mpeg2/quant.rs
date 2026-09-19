// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use super::tables::{DEFAULT_INTRA, ZIGZAG};
use super::vlc::DctTable;

const MAX_LEVEL: i32 = 2047;
const INTRA_BIAS: f32 = 0.375;

pub struct Quantiser {
    qs: i32,
    lambda: f32,
    trellis: bool,
}

#[derive(Clone, Copy)]
struct Node {
    pos: isize,
    cost: f32,
    level: i32,
    prev: usize,
}

impl Quantiser {
    pub fn new(code: u8, lambda: f32, trellis: bool) -> Self {
        Self {
            qs: 2 * code as i32,
            lambda,
            trellis,
        }
    }

    fn weight(inter: bool, j: usize) -> i32 {
        if inter { 16 } else { DEFAULT_INTRA[j] as i32 }
    }

    fn recon(&self, level: i32, weight: i32, inter: bool) -> i32 {
        (((2 * level + i32::from(inter)) * weight * self.qs) >> 5).min(MAX_LEVEL)
    }

    pub fn intra(&self, coef: &[f32; 64], table: &DctTable) -> [i16; 64] {
        let mut levels = self.ac(coef, false, table);
        levels[0] = (coef[0] / 8.0).round().clamp(0.0, 255.0) as i16;
        levels
    }

    pub fn inter(&self, coef: &[f32; 64], table: &DctTable) -> [i16; 64] {
        self.ac(coef, true, table)
    }

    fn ac(&self, coef: &[f32; 64], inter: bool, table: &DctTable) -> [i16; 64] {
        if self.trellis {
            return self.trellis(coef, inter, table);
        }
        let start = usize::from(!inter);
        let mut levels = [0i16; 64];
        for &j in &ZIGZAG[start..] {
            let step = (Self::weight(inter, j) * self.qs) as f32 / 16.0;
            let a = coef[j].abs() / step;
            let mag = if inter { a } else { a + INTRA_BIAS } as i32;
            let mag = mag.min(MAX_LEVEL);
            levels[j] = if coef[j] < 0.0 { -mag } else { mag } as i16;
        }
        levels
    }

    fn candidates(&self, f: f32, weight: i32, inter: bool) -> ([i32; 2], usize) {
        if 2.0 * f <= self.recon(1, weight, inter) as f32 {
            return ([0; 2], 0);
        }
        let step = (weight * self.qs) as f32 / 16.0;
        let mut lo = (if inter { f / step - 0.5 } else { f / step } as i32).clamp(1, MAX_LEVEL);
        while lo > 1 && self.recon(lo, weight, inter) as f32 > f {
            lo -= 1;
        }
        while lo < MAX_LEVEL && self.recon(lo + 1, weight, inter) as f32 <= f {
            lo += 1;
        }
        if lo == MAX_LEVEL {
            ([lo, 0], 1)
        } else {
            ([lo, lo + 1], 2)
        }
    }

    fn trellis(&self, coef: &[f32; 64], inter: bool, table: &DctTable) -> [i16; 64] {
        let start = usize::from(!inter);
        let mut zero = [0f32; 65];
        for i in 0..64 {
            let f = if i < start { 0.0 } else { coef[ZIGZAG[i]] };
            zero[i + 1] = zero[i] + f * f;
        }
        let gap = |from: isize, to: usize| zero[to] - zero[(from + 1) as usize];
        let mut nodes = vec![Node {
            pos: start as isize - 1,
            cost: 0.0,
            level: 0,
            prev: 0,
        }];
        let mut alive: Vec<usize> = vec![0];
        for (i, &j) in ZIGZAG.iter().enumerate().skip(start) {
            let f = coef[j].abs();
            let weight = Self::weight(inter, j);
            let (levels, count) = self.candidates(f, weight, inter);
            if count == 0 {
                continue;
            }
            let mut best_here = f32::INFINITY;
            for &level in &levels[..count] {
                let d = f - self.recon(level, weight, inter) as f32;
                let dist = d * d;
                let mut best = (f32::INFINITY, 0);
                for &n in &alive {
                    let node = &nodes[n];
                    let run = (i as isize - node.pos - 1) as usize;
                    let first = inter && n == 0;
                    let bits = table.bits(run, level, first) as f32;
                    let cost = node.cost + gap(node.pos, i) + dist + self.lambda * bits;
                    if cost < best.0 {
                        best = (cost, n);
                    }
                }
                let sign = if coef[j] < 0.0 { -1 } else { 1 };
                nodes.push(Node {
                    pos: i as isize,
                    cost: best.0,
                    level: sign * level,
                    prev: best.1,
                });
                best_here = best_here.min(best.0);
            }
            let fresh = nodes.len() - count..nodes.len();
            alive.retain(|&n| nodes[n].cost + gap(nodes[n].pos, i + 1) < best_here);
            alive.extend(fresh);
        }
        let eob = table.eob.1 as f32;
        let mut end = (f32::INFINITY, 0);
        for &n in &alive {
            let node = &nodes[n];
            let bits = if n == 0 && inter { 0.0 } else { eob };
            let cost = node.cost + gap(node.pos, 64) + self.lambda * bits;
            if cost < end.0 {
                end = (cost, n);
            }
        }
        let mut levels = [0i16; 64];
        let mut n = end.1;
        while n != 0 {
            let node = nodes[n];
            levels[ZIGZAG[node.pos as usize]] = node.level as i16;
            n = node.prev;
        }
        levels
    }

    pub fn dequantise(&self, levels: &[i16; 64], inter: bool) -> [i16; 64] {
        let mut out = [0i16; 64];
        let start = usize::from(!inter);
        if !inter {
            out[0] = levels[0] * 8;
        }
        for j in start..64 {
            let level = levels[j] as i32;
            if level != 0 {
                let c = self.recon(level.abs(), Self::weight(inter, j), inter);
                out[j] = if level < 0 { -c } else { c } as i16;
            }
        }
        let parity = out.iter().fold(0i16, |p, &c| p ^ c) & 1;
        if parity == 0 {
            out[63] ^= 1;
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mpeg2::vlc::dct;

    #[test]
    fn plain_levels_reconstruct_near_the_input() {
        let q = Quantiser::new(4, 0.0, false);
        let mut coef = [0f32; 64];
        coef[0] = 800.0;
        coef[1] = -100.0;
        coef[8] = 30.0;
        let levels = q.intra(&coef, dct(true));
        assert_eq!(levels[0], 100);
        let deq = q.dequantise(&levels, false);
        assert_eq!(deq[0], 800);
        assert!((deq[1] as f32 + 100.0).abs() <= 8.0);
        assert!((deq[8] as f32 - 30.0).abs() <= 8.0);
    }

    #[test]
    fn mismatch_control_makes_the_sum_odd() {
        let q = Quantiser::new(8, 0.0, false);
        let mut levels = [0i16; 64];
        levels[5] = 2;
        let deq = q.dequantise(&levels, true);
        assert_eq!(deq.iter().map(|&c| c as i32).sum::<i32>() & 1, 1);
    }

    #[test]
    fn trellis_without_rate_cost_keeps_the_nearest_levels() {
        let coef: [f32; 64] = std::array::from_fn(|i| ((i * 53) % 97) as f32 - 48.0);
        for inter in [false, true] {
            let q = Quantiser::new(2, 0.0, true);
            let table = dct(!inter);
            let t = q.ac(&coef, inter, table);
            for j in usize::from(!inter)..64 {
                let w = Quantiser::weight(inter, j);
                let err = |l: i32| {
                    (coef[j].abs() - if l == 0 { 0 } else { q.recon(l, w, inter) } as f32).abs()
                };
                let chosen = err(t[j].abs() as i32);
                for l in 0..40 {
                    assert!(chosen <= err(l) + 1e-3, "{inter} {j} {} vs {l}", t[j]);
                }
            }
        }
    }

    #[test]
    fn trellis_drops_expensive_isolated_coefficients() {
        let mut coef = [0f32; 64];
        coef[ZIGZAG[40]] = 20.0;
        let q = Quantiser::new(8, 0.85 * 64.0, true);
        assert!(q.inter(&coef, dct(false)).iter().all(|&l| l == 0));
    }
}
