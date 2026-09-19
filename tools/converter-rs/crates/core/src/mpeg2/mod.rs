// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod bits;
mod headers;
mod idct;
mod motion;
mod picture;
mod quant;
#[rustfmt::skip]
mod tables;
mod vlc;

use anyhow::{Result, bail};

use bits::BitWriter;
use motion::{Planes, Reference};
pub use picture::PictureType;

pub use crate::color::Matrix;
use crate::framerate::Rate;
use crate::jpeg::Frame;

#[derive(Clone, Debug)]
pub struct Settings {
    pub width: usize,
    pub height: usize,
    pub rate: Rate,
    pub qscale: u8,
    pub bframes: usize,
    pub hq: bool,
    pub matrix: Option<Matrix>,
}

impl Settings {
    fn coded(&self) -> (usize, usize) {
        (
            self.width.next_multiple_of(16),
            self.height.next_multiple_of(16),
        )
    }
}

pub struct Picture {
    pub data: Vec<u8>,
    pub index: usize,
    pub kind: PictureType,
    pub recon: Option<Vec<u8>>,
}

pub fn sequence_header(settings: &Settings) -> Vec<u8> {
    headers::sequence(settings)
}

pub fn timescale(rate: Rate) -> u32 {
    let mut scale = rate.num();
    while scale < 10_000 {
        scale *= 2;
    }
    scale as u32
}

pub fn reorder_delay(bframes: usize) -> u64 {
    u64::from(bframes > 0)
}

pub struct GopEncoder {
    settings: Settings,
    first_frame: u64,
    count: usize,
    anchor: Option<Reference>,
    pending: Vec<(usize, Planes)>,
    keep_recon: bool,
}

impl GopEncoder {
    pub fn new(settings: &Settings, first_frame: u64) -> Self {
        Self {
            settings: settings.clone(),
            first_frame,
            count: 0,
            anchor: None,
            pending: Vec::new(),
            keep_recon: false,
        }
    }

    pub fn keep_recon(mut self) -> Self {
        self.keep_recon = true;
        self
    }

    pub fn push(&mut self, frame: &Frame) -> Result<Vec<Picture>> {
        if (frame.width, frame.height) != (self.settings.width, self.settings.height) {
            bail!(
                "mpeg2: frame is {}x{}, expected {}x{}",
                frame.width,
                frame.height,
                self.settings.width,
                self.settings.height
            );
        }
        let (w, h) = self.settings.coded();
        let planes = Planes::from_frame(frame, w, h);
        let index = self.count;
        self.count += 1;
        if index == 0 {
            let (picture, reference) = self.encode(PictureType::I, 0, &planes, None, None);
            self.anchor = Some(reference.expect("an I picture is a reference"));
            return Ok(vec![picture]);
        }
        self.pending.push((index, planes));
        Ok(if self.pending.len() > self.settings.bframes {
            self.flush()
        } else {
            Vec::new()
        })
    }

    pub fn finish(mut self) -> Vec<Picture> {
        if self.pending.is_empty() {
            Vec::new()
        } else {
            self.flush()
        }
    }

    fn flush(&mut self) -> Vec<Picture> {
        let (index, planes) = self.pending.pop().expect("a pending frame");
        let previous = self
            .anchor
            .take()
            .expect("the GOP starts with an I picture");
        let (p, reference) = self.encode(PictureType::P, index, &planes, Some(&previous), None);
        let next = reference.expect("a P picture is a reference");
        let mut out = vec![p];
        for (index, planes) in std::mem::take(&mut self.pending) {
            let (b, _) = self.encode(PictureType::B, index, &planes, Some(&previous), Some(&next));
            out.push(b);
        }
        self.anchor = Some(next);
        out
    }

    fn encode(
        &self,
        kind: PictureType,
        index: usize,
        planes: &Planes,
        fwd: Option<&Reference>,
        bwd: Option<&Reference>,
    ) -> (Picture, Option<Reference>) {
        let mut w = BitWriter::default();
        if index == 0 {
            let header = headers::sequence(&self.settings);
            for &b in &header {
                bits::Sink::put(&mut w, b as u32, 8);
            }
            headers::gop(&mut w, self.first_frame, self.settings.rate);
        }
        let encoder = picture::Encoder::new(
            kind,
            planes,
            fwd,
            bwd,
            self.settings.qscale,
            self.settings.hq,
        );
        let recon = encoder.encode(&mut w, index);
        let cropped = self
            .keep_recon
            .then(|| recon.crop(self.settings.width, self.settings.height));
        let reference = (kind != PictureType::B).then(|| Reference::new(recon));
        (
            Picture {
                data: w.finish(),
                index,
                kind,
                recon: cropped,
            },
            reference,
        )
    }
}
