// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::io::{Read, Write};

use tab5conv_wasm::MjpegWorker;

fn main() {
    let args: Vec<usize> = std::env::args()
        .skip(1)
        .map(|a| a.parse().expect("width height quality"))
        .collect();
    let [width, height, quality] = args[..] else {
        panic!("usage: jpeg WIDTH HEIGHT QUALITY < rgba > jpeg");
    };
    let mut rgba = Vec::new();
    std::io::stdin().read_to_end(&mut rgba).unwrap();
    let mut worker = MjpegWorker::new();
    worker.analyze(0, &rgba, width, height);
    let frame = worker
        .encode(0, quality as u8, 30, 1 << 20, true)
        .unwrap_or_else(|_| panic!("encode failed"));
    std::io::stdout().write_all(&frame.data()).unwrap();
}
