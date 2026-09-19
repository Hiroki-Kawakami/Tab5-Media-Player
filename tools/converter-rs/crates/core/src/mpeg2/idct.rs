// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

const W1: i32 = 22725;
const W2: i32 = 21407;
const W3: i32 = 19266;
const W4: i32 = 16383;
const W5: i32 = 12873;
const W6: i32 = 8867;
const W7: i32 = 4520;
const ROW_SHIFT: u32 = 11;
const COL_SHIFT: u32 = 20;

fn row(r: &mut [i16]) {
    if r[1..].iter().all(|&v| v == 0) {
        let dc = ((r[0] as i32) << 3) as i16;
        r.fill(dc);
        return;
    }
    let x: [i32; 8] = std::array::from_fn(|i| r[i] as i32);
    let mut a0 = (W4 * x[0]).wrapping_add(1 << (ROW_SHIFT - 1));
    let (mut a1, mut a2, mut a3) = (a0, a0, a0);
    a0 = a0.wrapping_add(W2 * x[2]);
    a1 = a1.wrapping_add(W6 * x[2]);
    a2 = a2.wrapping_sub(W6 * x[2]);
    a3 = a3.wrapping_sub(W2 * x[2]);
    let mut b0 = (W1 * x[1]).wrapping_add(W3 * x[3]);
    let mut b1 = (W3 * x[1]).wrapping_sub(W7 * x[3]);
    let mut b2 = (W5 * x[1]).wrapping_sub(W1 * x[3]);
    let mut b3 = (W7 * x[1]).wrapping_sub(W5 * x[3]);
    a0 = a0.wrapping_add(W4 * x[4] + W6 * x[6]);
    a1 = a1.wrapping_add(-W4 * x[4] - W2 * x[6]);
    a2 = a2.wrapping_add(-W4 * x[4] + W2 * x[6]);
    a3 = a3.wrapping_add(W4 * x[4] - W6 * x[6]);
    b0 = b0.wrapping_add(W5 * x[5] + W7 * x[7]);
    b1 = b1.wrapping_add(-W1 * x[5] - W5 * x[7]);
    b2 = b2.wrapping_add(W7 * x[5] + W3 * x[7]);
    b3 = b3.wrapping_add(W3 * x[5] - W1 * x[7]);
    r[0] = (a0.wrapping_add(b0) >> ROW_SHIFT) as i16;
    r[7] = (a0.wrapping_sub(b0) >> ROW_SHIFT) as i16;
    r[1] = (a1.wrapping_add(b1) >> ROW_SHIFT) as i16;
    r[6] = (a1.wrapping_sub(b1) >> ROW_SHIFT) as i16;
    r[2] = (a2.wrapping_add(b2) >> ROW_SHIFT) as i16;
    r[5] = (a2.wrapping_sub(b2) >> ROW_SHIFT) as i16;
    r[3] = (a3.wrapping_add(b3) >> ROW_SHIFT) as i16;
    r[4] = (a3.wrapping_sub(b3) >> ROW_SHIFT) as i16;
}

fn column(b: &[i16; 64], c: usize) -> [i32; 8] {
    let x: [i32; 8] = std::array::from_fn(|i| b[c + 8 * i] as i32);
    let mut a0 = W4.wrapping_mul(x[0] + (1 << (COL_SHIFT - 1)) / W4);
    let (mut a1, mut a2, mut a3) = (a0, a0, a0);
    a0 = a0.wrapping_add(W2 * x[2]);
    a1 = a1.wrapping_add(W6 * x[2]);
    a2 = a2.wrapping_sub(W6 * x[2]);
    a3 = a3.wrapping_sub(W2 * x[2]);
    let mut b0 = W1 * x[1];
    let mut b1 = W3 * x[1];
    let mut b2 = W5 * x[1];
    let mut b3 = W7 * x[1];
    b0 = b0.wrapping_add(W3 * x[3]);
    b1 = b1.wrapping_sub(W7 * x[3]);
    b2 = b2.wrapping_sub(W1 * x[3]);
    b3 = b3.wrapping_sub(W5 * x[3]);
    a0 = a0.wrapping_add(W4 * x[4]);
    a1 = a1.wrapping_sub(W4 * x[4]);
    a2 = a2.wrapping_sub(W4 * x[4]);
    a3 = a3.wrapping_add(W4 * x[4]);
    b0 = b0.wrapping_add(W5 * x[5]);
    b1 = b1.wrapping_sub(W1 * x[5]);
    b2 = b2.wrapping_add(W7 * x[5]);
    b3 = b3.wrapping_add(W3 * x[5]);
    a0 = a0.wrapping_add(W6 * x[6]);
    a1 = a1.wrapping_sub(W2 * x[6]);
    a2 = a2.wrapping_add(W2 * x[6]);
    a3 = a3.wrapping_sub(W6 * x[6]);
    b0 = b0.wrapping_add(W7 * x[7]);
    b1 = b1.wrapping_sub(W5 * x[7]);
    b2 = b2.wrapping_add(W3 * x[7]);
    b3 = b3.wrapping_sub(W1 * x[7]);
    [
        a0.wrapping_add(b0) >> COL_SHIFT,
        a1.wrapping_add(b1) >> COL_SHIFT,
        a2.wrapping_add(b2) >> COL_SHIFT,
        a3.wrapping_add(b3) >> COL_SHIFT,
        a3.wrapping_sub(b3) >> COL_SHIFT,
        a2.wrapping_sub(b2) >> COL_SHIFT,
        a1.wrapping_sub(b1) >> COL_SHIFT,
        a0.wrapping_sub(b0) >> COL_SHIFT,
    ]
}

pub fn inverse(mut block: [i16; 64]) -> [i32; 64] {
    for r in block.chunks_exact_mut(8) {
        row(r);
    }
    let mut out = [0; 64];
    for c in 0..8 {
        for (r, v) in column(&block, c).into_iter().enumerate() {
            out[r * 8 + c] = v;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dc_only_is_flat() {
        let mut b = [0i16; 64];
        b[0] = 8 * 100;
        assert!(inverse(b).iter().all(|&v| v == 100));
    }

    #[test]
    fn inverts_the_forward_transform() {
        let pixels: [f32; 64] = std::array::from_fn(|i| ((i * 37) % 255) as f32 - 128.0);
        let coefficients = crate::jpeg::dct::forward(&pixels);
        let out = inverse(coefficients.map(|c| c.round() as i16));
        for (a, b) in out.iter().zip(pixels) {
            assert!((*a as f32 - b).abs() <= 1.0, "{a} {b}");
        }
    }
}
