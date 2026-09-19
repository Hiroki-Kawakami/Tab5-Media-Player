// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::sync::OnceLock;

fn basis() -> &'static [[f32; 8]; 8] {
    static BASIS: OnceLock<[[f32; 8]; 8]> = OnceLock::new();
    BASIS.get_or_init(|| {
        let mut c = [[0.0f32; 8]; 8];
        for (u, row) in c.iter_mut().enumerate() {
            let scale = if u == 0 { (1.0f64 / 8.0).sqrt() } else { 0.5 };
            for (x, value) in row.iter_mut().enumerate() {
                let angle = (2 * x + 1) as f64 * u as f64 * std::f64::consts::PI / 16.0;
                *value = (scale * angle.cos()) as f32;
            }
        }
        c
    })
}

pub fn forward(block: &[f32; 64]) -> [f32; 64] {
    let c = basis();
    let mut rows = [0.0f32; 64];
    for y in 0..8 {
        for u in 0..8 {
            rows[y * 8 + u] = (0..8).map(|x| c[u][x] * block[y * 8 + x]).sum();
        }
    }
    let mut out = [0.0f32; 64];
    for v in 0..8 {
        for u in 0..8 {
            out[v * 8 + u] = (0..8).map(|y| c[v][y] * rows[y * 8 + u]).sum();
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flat_block_has_only_dc() {
        let out = forward(&[10.0; 64]);
        assert!((out[0] - 80.0).abs() < 1e-3);
        assert!(out[1..].iter().all(|v| v.abs() < 1e-3));
    }

    #[test]
    fn matches_the_jpeg_definition() {
        let block: [f32; 64] = std::array::from_fn(|i| ((i * 37) % 255) as f32 - 128.0);
        let out = forward(&block);
        for v in 0..8 {
            for u in 0..8 {
                let cu = if u == 0 { 1.0 / 2f64.sqrt() } else { 1.0 };
                let cv = if v == 0 { 1.0 / 2f64.sqrt() } else { 1.0 };
                let mut sum = 0.0f64;
                for y in 0..8 {
                    for x in 0..8 {
                        sum += block[y * 8 + x] as f64
                            * (((2 * x + 1) * u) as f64 * std::f64::consts::PI / 16.0).cos()
                            * (((2 * y + 1) * v) as f64 * std::f64::consts::PI / 16.0).cos();
                    }
                }
                let expected = 0.25 * cu * cv * sum;
                assert!((out[v * 8 + u] as f64 - expected).abs() < 1e-2);
            }
        }
    }
}
