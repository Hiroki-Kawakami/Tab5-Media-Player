// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use crate::jpeg::Frame;

pub const MAX_MV: i32 = 127;

pub type Mv = [i32; 2];

#[derive(Clone)]
pub struct Planes {
    pub width: usize,
    pub height: usize,
    pub y: Vec<u8>,
    pub u: Vec<u8>,
    pub v: Vec<u8>,
}

fn pad(src: &[u8], sw: usize, sh: usize, w: usize, h: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(w * h);
    for y in 0..h {
        let row = &src[y.min(sh - 1) * sw..][..sw];
        out.extend_from_slice(row);
        out.resize(out.len() + (w - sw), row[sw - 1]);
    }
    out
}

impl Planes {
    pub fn from_frame(frame: &Frame, width: usize, height: usize) -> Self {
        let (cw, ch) = (frame.width / 2, frame.height / 2);
        Self {
            width,
            height,
            y: pad(frame.y, frame.width, frame.height, width, height),
            u: pad(frame.cb, cw, ch, width / 2, height / 2),
            v: pad(frame.cr, cw, ch, width / 2, height / 2),
        }
    }

    pub fn blank(width: usize, height: usize) -> Self {
        Self {
            width,
            height,
            y: vec![0; width * height],
            u: vec![0; width * height / 4],
            v: vec![0; width * height / 4],
        }
    }

    pub fn crop(&self, width: usize, height: usize) -> Vec<u8> {
        let mut out = Vec::with_capacity(width * height * 3 / 2);
        for (plane, w, h, stride) in [
            (&self.y, width, height, self.width),
            (&self.u, width / 2, height / 2, self.width / 2),
            (&self.v, width / 2, height / 2, self.width / 2),
        ] {
            for row in plane.chunks_exact(stride).take(h) {
                out.extend_from_slice(&row[..w]);
            }
        }
        out
    }
}

pub struct Reference {
    pub planes: Planes,
    half: [Vec<u8>; 3],
}

impl Reference {
    pub fn new(planes: Planes) -> Self {
        let (w, h) = (planes.width, planes.height);
        let p = &planes.y;
        let at = |x: usize, y: usize| p[y.min(h - 1) * w + x.min(w - 1)] as u32;
        let mut half = [vec![0; w * h], vec![0; w * h], vec![0; w * h]];
        for y in 0..h {
            for x in 0..w {
                let (a, b, c, d) = (at(x, y), at(x + 1, y), at(x, y + 1), at(x + 1, y + 1));
                half[0][y * w + x] = ((a + b + 1) >> 1) as u8;
                half[1][y * w + x] = ((a + c + 1) >> 1) as u8;
                half[2][y * w + x] = ((a + b + c + d + 2) >> 2) as u8;
            }
        }
        Self { planes, half }
    }

    fn luma(&self, hx: i32, hy: i32) -> &[u8] {
        match (hx, hy) {
            (0, 0) => &self.planes.y,
            (1, 0) => &self.half[0],
            (0, 1) => &self.half[1],
            _ => &self.half[2],
        }
    }

    pub fn valid(&self, mbx: usize, mby: usize, mv: Mv) -> bool {
        let inside = |pos: usize, v: i32, size: usize, extent: usize| {
            let start = pos as i32 + (v >> 1);
            start >= 0 && start + (extent as i32) + (v & 1) <= size as i32
        };
        let (w, h) = (self.planes.width, self.planes.height);
        mv[0].abs() <= MAX_MV
            && mv[1].abs() <= MAX_MV
            && inside(mbx * 16, mv[0], w, 16)
            && inside(mby * 16, mv[1], h, 16)
            && inside(mbx * 8, mv[0] / 2, w / 2, 8)
            && inside(mby * 8, mv[1] / 2, h / 2, 8)
    }

    pub fn predict(&self, mbx: usize, mby: usize, mv: Mv, out: &mut [u8; 384]) {
        let w = self.planes.width;
        let x = (mbx * 16) as i32 + (mv[0] >> 1);
        let y = (mby * 16) as i32 + (mv[1] >> 1);
        let plane = self.luma(mv[0] & 1, mv[1] & 1);
        for r in 0..16 {
            let start = (y as usize + r) * w + x as usize;
            out[r * 16..r * 16 + 16].copy_from_slice(&plane[start..start + 16]);
        }
        let (cx, cy) = (mv[0] / 2, mv[1] / 2);
        let (hx, hy) = (cx & 1, cy & 1);
        let x = (mbx * 8) as i32 + (cx >> 1);
        let y = (mby * 8) as i32 + (cy >> 1);
        let cw = w / 2;
        for (plane, base) in [(&self.planes.u, 256), (&self.planes.v, 320)] {
            let at = |c: i32, r: i32| plane[(y + r) as usize * cw + (x + c) as usize] as u32;
            for r in 0..8 {
                for c in 0..8 {
                    let a = at(c, r);
                    let v = match (hx, hy) {
                        (0, 0) => a,
                        (1, 0) => (a + at(c + 1, r) + 1) >> 1,
                        (0, 1) => (a + at(c, r + 1) + 1) >> 1,
                        _ => (a + at(c + 1, r) + at(c, r + 1) + at(c + 1, r + 1) + 2) >> 2,
                    };
                    out[base + (r * 8 + c) as usize] = v as u8;
                }
            }
        }
    }

    fn sad(&self, cur: &[u8; 256], mbx: usize, mby: usize, mv: Mv, limit: u32) -> u32 {
        let w = self.planes.width;
        let x = (mbx * 16) as i32 + (mv[0] >> 1);
        let y = (mby * 16) as i32 + (mv[1] >> 1);
        let plane = self.luma(mv[0] & 1, mv[1] & 1);
        let mut sum = 0;
        for r in 0..16 {
            let start = (y as usize + r) * w + x as usize;
            let row = &plane[start..start + 16];
            sum += row
                .iter()
                .zip(&cur[r * 16..r * 16 + 16])
                .map(|(&a, &b)| a.abs_diff(b) as u32)
                .sum::<u32>();
            if sum >= limit {
                return sum;
            }
        }
        sum
    }
}

pub fn luma_block(planes: &Planes, mbx: usize, mby: usize) -> [u8; 256] {
    let w = planes.width;
    let mut out = [0; 256];
    for r in 0..16 {
        let start = (mby * 16 + r) * w + mbx * 16;
        out[r * 16..r * 16 + 16].copy_from_slice(&planes.y[start..start + 16]);
    }
    out
}

pub fn mv_bits(d: i32) -> u32 {
    if d == 0 {
        1
    } else {
        2 * (32 - d.unsigned_abs().leading_zeros()) + 2
    }
}

pub struct Search<'a> {
    pub reference: &'a Reference,
    pub lambda: u32,
}

const LARGE: [Mv; 8] = [
    [0, -4],
    [2, -2],
    [4, 0],
    [2, 2],
    [0, 4],
    [-2, 2],
    [-4, 0],
    [-2, -2],
];
const SMALL: [Mv; 4] = [[0, -2], [2, 0], [0, 2], [-2, 0]];
const HALF: [Mv; 8] = [
    [-1, -1],
    [0, -1],
    [1, -1],
    [-1, 0],
    [1, 0],
    [-1, 1],
    [0, 1],
    [1, 1],
];

impl Search<'_> {
    fn cost(&self, cur: &[u8; 256], mbx: usize, mby: usize, mv: Mv, pred: Mv, limit: u32) -> u32 {
        let rate = self.lambda * (mv_bits(mv[0] - pred[0]) + mv_bits(mv[1] - pred[1]));
        if rate >= limit {
            return u32::MAX;
        }
        rate + self.reference.sad(cur, mbx, mby, mv, limit - rate)
    }

    pub fn run(
        &self,
        cur: &[u8; 256],
        mbx: usize,
        mby: usize,
        starts: &[Mv],
        pred: Mv,
    ) -> (Mv, u32) {
        let mut best = ([0, 0], self.cost(cur, mbx, mby, [0, 0], pred, u32::MAX));
        let try_mv = |mv: Mv, best: &mut (Mv, u32)| {
            if mv != best.0 && self.reference.valid(mbx, mby, mv) {
                let c = self.cost(cur, mbx, mby, mv, pred, best.1);
                if c < best.1 {
                    *best = (mv, c);
                }
            }
        };
        for &s in starts {
            try_mv([s[0] & !1, s[1] & !1], &mut best);
        }
        for radius in [16, 32, 64] {
            let centre = best.0;
            for d in LARGE {
                try_mv(
                    [centre[0] + d[0] * radius / 4, centre[1] + d[1] * radius / 4],
                    &mut best,
                );
            }
        }
        for pattern in [&LARGE[..], &SMALL[..]] {
            for _ in 0..32 {
                let centre = best.0;
                for d in pattern {
                    try_mv([centre[0] + d[0], centre[1] + d[1]], &mut best);
                }
                if best.0 == centre {
                    break;
                }
            }
        }
        let centre = best.0;
        for d in HALF {
            try_mv([centre[0] + d[0], centre[1] + d[1]], &mut best);
        }
        let sad =
            best.1 - self.lambda * (mv_bits(best.0[0] - pred[0]) + mv_bits(best.0[1] - pred[1]));
        (best.0, sad)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn textured(w: usize, h: usize, dx: usize, dy: usize) -> Planes {
        let mut p = Planes::blank(w, h);
        for y in 0..h {
            for x in 0..w {
                let (sx, sy) = (x + dx, y + dy);
                p.y[y * w + x] = ((sx * 7 + sy * 13 + (sx * sy) % 31) % 251) as u8;
            }
        }
        p
    }

    #[test]
    fn finds_a_translation() {
        let reference = Reference::new(textured(96, 64, 0, 0));
        let cur = textured(96, 64, 5, 3);
        let search = Search {
            reference: &reference,
            lambda: 1,
        };
        let block = luma_block(&cur, 2, 1);
        let (mv, sad) = search.run(&block, 2, 1, &[], [0, 0]);
        assert_eq!((mv, sad), ([10, 6], 0));
    }

    #[test]
    fn prediction_stays_inside_the_picture() {
        let reference = Reference::new(textured(32, 32, 0, 0));
        assert!(reference.valid(1, 1, [-32, -32]));
        assert!(!reference.valid(1, 1, [-33, 0]));
        assert!(!reference.valid(1, 1, [1, 0]));
        assert!(reference.valid(0, 0, [31, 31]));
        let mut out = [0; 384];
        reference.predict(0, 0, [31, 31], &mut out);
    }
}
