// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::io::{Read, Write};

use tab5conv_wasm::mpeg2::YuvScaler;

fn main() {
    let args: Vec<f64> = std::env::args()
        .skip(1)
        .map(|a| a.parse().expect("numbers"))
        .collect();
    let [width, height, ref geometry @ .., matrix, full_range] = args[..] else {
        panic!("usage: yuv WIDTH HEIGHT GEOMETRY(9) MATRIX FULL_RANGE < i420 > i420");
    };
    let (w, h) = (width as u32, height as u32);
    let mut data = Vec::new();
    std::io::stdin().read_to_end(&mut data).unwrap();
    let luma = w * h;
    let chroma = (w / 2) * (h / 2);
    let layout = [w, h, 0, w, luma, w / 2, luma + chroma, w / 2];
    let frame = (luma + 2 * chroma) as usize;
    let mut scaler = YuvScaler::new(geometry.to_vec()).unwrap_or_else(|_| panic!("bad geometry"));
    let mut out = std::io::stdout().lock();
    for f in data.chunks_exact(frame) {
        let converted = scaler
            .convert("I420", f, &layout, &[matrix as u8, full_range as u8])
            .unwrap_or_else(|_| panic!("convert failed"));
        out.write_all(&converted).unwrap();
    }
}
