// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::io::{Read, Write};

use tab5conv_wasm::mpeg2::{EncodedPictures, Mpeg2Worker};

fn main() {
    let config: Vec<f64> = std::env::args()
        .skip(1)
        .map(|a| a.parse().expect("numbers"))
        .collect();
    let [width, height, .., keyint] = config[..] else {
        panic!("usage: mpeg2 CONFIG(9) < i420 > pictures");
    };
    let mut data = Vec::new();
    std::io::stdin().read_to_end(&mut data).unwrap();
    let frame = (width * height * 1.5) as usize;
    let mut worker = Mpeg2Worker::new(config.clone()).unwrap_or_else(|_| panic!("bad config"));
    let mut out = std::io::stdout().lock();
    let mut write = |p: EncodedPictures| out.write_all(&p.data()).unwrap();
    let count = data.len() / frame;
    for (i, f) in data.chunks_exact(frame).enumerate() {
        let gop = (i / keyint as usize) as u32;
        write(
            worker
                .push(gop, f)
                .unwrap_or_else(|_| panic!("push failed")),
        );
        if (i + 1) % keyint as usize == 0 || i + 1 == count {
            write(worker.finish(gop));
        }
    }
}
