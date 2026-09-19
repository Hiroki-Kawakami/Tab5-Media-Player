// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::io::{Read, Write};

use tab5conv_core::framerate::Rate;
use tab5conv_core::jpeg::Frame;
use tab5conv_core::mpeg2::{GopEncoder, Matrix, Picture, Settings};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 8 {
        panic!(
            "usage: mpeg2 WIDTH HEIGHT FPS QSCALE BFRAMES KEYINT HQ OUT.m2v [RECON.yuv] < yuv420p"
        );
    }
    let num = |i: usize| args[i].parse::<usize>().expect("a number");
    let settings = Settings {
        width: num(0),
        height: num(1),
        rate: Rate::new(num(2) as u64, 1).expect("a frame rate"),
        qscale: num(3) as u8,
        bframes: num(4),
        hq: num(6) != 0,
        matrix: Some(Matrix::Bt601),
    };
    let keyint = num(5);
    let size = Frame::bytes(settings.width, settings.height);
    let mut input = std::io::stdin().lock();
    let mut out = std::fs::File::create(&args[7]).unwrap();
    let mut recon: Vec<Option<Vec<u8>>> = Vec::new();
    let mut buf = vec![0; size];
    let mut frame = 0usize;
    let mut encoder: Option<GopEncoder> = None;
    let mut write = |pictures: Vec<Picture>, first: usize, recon: &mut Vec<Option<Vec<u8>>>| {
        for p in pictures {
            out.write_all(&p.data).unwrap();
            let at = first + p.index;
            recon.resize(recon.len().max(at + 1), None);
            recon[at] = p.recon;
        }
    };
    let mut gop_first = 0;
    while input.read_exact(&mut buf).is_ok() {
        if frame.is_multiple_of(keyint) {
            if let Some(e) = encoder.take() {
                write(e.finish(), gop_first, &mut recon);
            }
            gop_first = frame;
            encoder = Some(GopEncoder::new(&settings, frame as u64).keep_recon());
        }
        let f = Frame::from_yuv420p(&buf, settings.width, settings.height);
        let pictures = encoder.as_mut().unwrap().push(&f).unwrap();
        write(pictures, gop_first, &mut recon);
        frame += 1;
    }
    if let Some(e) = encoder.take() {
        write(e.finish(), gop_first, &mut recon);
    }
    if let Some(path) = args.get(8) {
        let mut f = std::fs::File::create(path).unwrap();
        for r in recon {
            f.write_all(&r.expect("every frame was encoded")).unwrap();
        }
    }
}
