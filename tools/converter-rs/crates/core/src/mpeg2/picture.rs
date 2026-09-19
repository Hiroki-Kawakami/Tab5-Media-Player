// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use super::bits::{BitWriter, Counter, Sink};
use super::idct;
use super::motion::{Mv, Planes, Reference, Search, luma_block, mv_bits};
use super::quant::Quantiser;
use super::tables::ZIGZAG;
use super::vlc::{self, MB_BWD, MB_FWD, MB_INTRA, MB_PATTERN};
use crate::jpeg::dct;

const UNUSED_F_CODE: u32 = 15;
const INTRA_BIAS: u32 = 500;
const RD_MODES: usize = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PictureType {
    I = 1,
    P = 2,
    B = 3,
}

#[derive(Clone, Copy, PartialEq)]
enum Mode {
    Intra,
    Inter { flags: u8, mv: [Mv; 2] },
}

#[derive(Clone)]
struct SliceState {
    pmv: [Mv; 2],
    dc: [i32; 3],
    prev: Option<(u8, [Mv; 2])>,
}

impl SliceState {
    fn new() -> Self {
        Self {
            pmv: [[0; 2]; 2],
            dc: [128; 3],
            prev: None,
        }
    }

    fn skip(&mut self, kind: PictureType) {
        self.dc = [128; 3];
        if kind == PictureType::P {
            self.pmv = [[0; 2]; 2];
            self.prev = Some((MB_FWD, [[0; 2]; 2]));
        }
    }

    fn skip_mode(&self, kind: PictureType) -> Option<Mode> {
        match kind {
            PictureType::I => None,
            PictureType::P => Some(Mode::Inter {
                flags: MB_FWD,
                mv: [[0; 2]; 2],
            }),
            PictureType::B => self.prev.map(|(flags, _)| Mode::Inter {
                flags,
                mv: [MB_FWD, MB_BWD].map(|dir| {
                    let s = usize::from(dir == MB_BWD);
                    if flags & dir != 0 {
                        self.pmv[s]
                    } else {
                        [0; 2]
                    }
                }),
            }),
        }
    }
}

struct Coded {
    mode: Mode,
    levels: [[i16; 64]; 6],
    cbp: u8,
    recon: [u8; 384],
    sse: u64,
}

pub struct Encoder<'a> {
    kind: PictureType,
    cur: &'a Planes,
    refs: [Option<&'a Reference>; 2],
    quant: Quantiser,
    code: u8,
    lambda: f32,
    lambda_sad: u32,
    hq: bool,
    r_size: [[u32; 2]; 2],
    mb_w: usize,
    mb_h: usize,
    best: [Vec<(Mv, u32)>; 2],
}

const BLOCKS: [[u16; 64]; 6] = {
    let mut out = [[0; 64]; 6];
    let mut b = 0;
    while b < 6 {
        let mut i = 0;
        while i < 64 {
            out[b][i] = match b {
                0..4 => ((b >> 1) * 8 + i / 8) * 16 + (b & 1) * 8 + i % 8,
                4 => 256 + i,
                _ => 320 + i,
            } as u16;
            i += 1;
        }
        b += 1;
    }
    out
};

fn block(b: usize) -> impl Iterator<Item = usize> {
    BLOCKS[b].iter().map(|&k| k as usize)
}

fn f_code_for(extent: i32) -> u32 {
    (0..4).find(|&r| extent < (16 << r)).unwrap_or(3)
}

fn wrap(delta: i32, r_size: u32) -> i32 {
    let f = 1 << r_size;
    if delta < -16 * f {
        delta + 32 * f
    } else if delta > 16 * f - 1 {
        delta - 32 * f
    } else {
        delta
    }
}

impl<'a> Encoder<'a> {
    pub fn new(
        kind: PictureType,
        cur: &'a Planes,
        fwd: Option<&'a Reference>,
        bwd: Option<&'a Reference>,
        code: u8,
        hq: bool,
    ) -> Self {
        let lambda = 0.85 * (code as f32).powi(2);
        Self {
            kind,
            cur,
            refs: [fwd, bwd],
            quant: Quantiser::new(code, lambda, hq),
            code,
            lambda,
            lambda_sad: ((lambda.sqrt()).round() as u32).max(1),
            hq,
            r_size: [[0; 2]; 2],
            mb_w: cur.width / 16,
            mb_h: cur.height / 16,
            best: [Vec::new(), Vec::new()],
        }
    }

    fn estimate(&mut self) {
        for s in 0..2 {
            let Some(reference) = self.refs[s] else {
                continue;
            };
            let search = Search {
                reference,
                lambda: self.lambda_sad,
            };
            let mut best: Vec<(Mv, u32)> = Vec::with_capacity(self.mb_w * self.mb_h);
            for mby in 0..self.mb_h {
                for mbx in 0..self.mb_w {
                    let at = |x: usize, y: usize| best[y * self.mb_w + x].0;
                    let mut starts = Vec::with_capacity(3);
                    let pred = if mbx > 0 { at(mbx - 1, mby) } else { [0, 0] };
                    starts.push(pred);
                    if mby > 0 {
                        starts.push(at(mbx, mby - 1));
                        if mbx + 1 < self.mb_w {
                            starts.push(at(mbx + 1, mby - 1));
                        }
                    }
                    let block = luma_block(self.cur, mbx, mby);
                    best.push(search.run(&block, mbx, mby, &starts, pred));
                }
            }
            for t in 0..2 {
                let extent = best
                    .iter()
                    .map(|(mv, _)| if mv[t] < 0 { -mv[t] - 1 } else { mv[t] })
                    .max()
                    .unwrap_or(0);
                self.r_size[s][t] = f_code_for(extent);
            }
            self.best[s] = best;
        }
    }

    fn header(&self, w: &mut BitWriter, temporal_reference: usize) {
        w.start_code(0x00);
        w.put(temporal_reference as u32 & 0x3FF, 10);
        w.put(self.kind as u32, 3);
        w.put(0xFFFF, 16);
        if self.kind != PictureType::I {
            w.put(0b0111, 4);
        }
        if self.kind == PictureType::B {
            w.put(0b0111, 4);
        }
        w.put(0, 1);
        w.start_code(0xB5);
        w.put(8, 4);
        for s in 0..2 {
            for t in 0..2 {
                let used = self.refs[s].is_some();
                w.put(
                    if used {
                        self.r_size[s][t] + 1
                    } else {
                        UNUSED_F_CODE
                    },
                    4,
                );
            }
        }
        w.put(0, 2);
        w.put(3, 2);
        for bit in [0, 1, 0, 0, 1, 0, 0, 1, 1, 0] {
            w.put(bit, 1);
        }
    }

    pub fn encode(mut self, w: &mut BitWriter, temporal_reference: usize) -> Planes {
        self.estimate();
        self.header(w, temporal_reference);
        let mut recon = Planes::blank(self.cur.width, self.cur.height);
        for mby in 0..self.mb_h {
            w.start_code(mby as u8 + 1);
            w.put(self.code as u32, 5);
            w.put(0, 1);
            let mut state = SliceState::new();
            let mut skipped = 0;
            for mbx in 0..self.mb_w {
                let src = self.source(mbx, mby);
                let edge = mbx == 0 || mbx + 1 == self.mb_w;
                let (coded, skip) = self.decide(&src, mbx, mby, &state, !edge);
                if skip {
                    state.skip(self.kind);
                    skipped += 1;
                } else {
                    self.code_mb(w, &mut state, &coded, skipped + 1);
                    skipped = 0;
                }
                self.store(&mut recon, mbx, mby, &coded.recon);
            }
        }
        recon
    }

    fn source(&self, mbx: usize, mby: usize) -> [u8; 384] {
        let mut out = [0; 384];
        out[..256].copy_from_slice(&luma_block(self.cur, mbx, mby));
        let cw = self.cur.width / 2;
        for r in 0..8 {
            let start = (mby * 8 + r) * cw + mbx * 8;
            out[256 + r * 8..264 + r * 8].copy_from_slice(&self.cur.u[start..start + 8]);
            out[320 + r * 8..328 + r * 8].copy_from_slice(&self.cur.v[start..start + 8]);
        }
        out
    }

    fn store(&self, recon: &mut Planes, mbx: usize, mby: usize, px: &[u8; 384]) {
        let w = recon.width;
        for r in 0..16 {
            let start = (mby * 16 + r) * w + mbx * 16;
            recon.y[start..start + 16].copy_from_slice(&px[r * 16..r * 16 + 16]);
        }
        let cw = w / 2;
        for r in 0..8 {
            let start = (mby * 8 + r) * cw + mbx * 8;
            recon.u[start..start + 8].copy_from_slice(&px[256 + r * 8..264 + r * 8]);
            recon.v[start..start + 8].copy_from_slice(&px[320 + r * 8..328 + r * 8]);
        }
    }

    fn predict(&self, mbx: usize, mby: usize, flags: u8, mv: &[Mv; 2]) -> [u8; 384] {
        let mut out = [0; 384];
        let first = usize::from(flags & MB_FWD == 0);
        let reference = |s: usize| self.refs[s].expect("reference for the prediction");
        reference(first).predict(mbx, mby, mv[first], &mut out);
        if flags & (MB_FWD | MB_BWD) == MB_FWD | MB_BWD {
            let mut other = [0; 384];
            reference(1).predict(mbx, mby, mv[1], &mut other);
            for (a, b) in out.iter_mut().zip(other) {
                *a = ((*a as u32 + b as u32 + 1) >> 1) as u8;
            }
        }
        out
    }

    fn usable(&self, mode: Mode, mbx: usize, mby: usize) -> bool {
        let Mode::Inter { flags, mv } = mode else {
            return true;
        };
        [MB_FWD, MB_BWD].into_iter().enumerate().all(|(s, dir)| {
            flags & dir == 0 || self.refs[s].is_some_and(|r| r.valid(mbx, mby, mv[s]))
        })
    }

    fn evaluate(&self, src: &[u8; 384], mode: Mode, mbx: usize, mby: usize) -> Coded {
        let intra = mode == Mode::Intra;
        let pred = match mode {
            Mode::Intra => [0; 384],
            Mode::Inter { flags, mv } => self.predict(mbx, mby, flags, &mv),
        };
        let mut coded = Coded {
            mode,
            levels: [[0; 64]; 6],
            cbp: 0,
            recon: pred,
            sse: 0,
        };
        let zero_below = self.quant.inter_zero_below();
        for b in 0..6 {
            let mut pixels = [0f32; 64];
            for (i, k) in block(b).enumerate() {
                pixels[i] = src[k] as f32 - if intra { 0.0 } else { pred[k] as f32 };
            }
            if !intra {
                let sad: f32 = pixels.iter().map(|p| p.abs()).sum();
                if sad * 0.251 < zero_below {
                    continue;
                }
            }
            let coef = dct::forward(&pixels);
            let levels = if intra {
                self.quant.intra(&coef, vlc::dct(true))
            } else {
                self.quant.inter(&coef, vlc::dct(false))
            };
            if !intra && levels.iter().all(|&l| l == 0) {
                continue;
            }
            coded.cbp |= 32 >> b;
            coded.levels[b] = levels;
            let residual = idct::inverse(self.quant.dequantise(&levels, !intra));
            for (i, k) in block(b).enumerate() {
                let base = if intra { 0 } else { pred[k] as i32 };
                coded.recon[k] = (base + residual[i]).clamp(0, 255) as u8;
            }
        }
        coded.sse = sse(src, &coded.recon);
        coded
    }

    fn bits(&self, state: &SliceState, coded: &Coded) -> u32 {
        let mut counter = Counter::default();
        self.code_mb(&mut counter, &mut state.clone(), coded, 1);
        counter.0
    }

    fn inter_modes(&self, mbx: usize, mby: usize) -> Vec<Mode> {
        let i = mby * self.mb_w + mbx;
        let fwd = self.best[0].get(i).map(|b| b.0);
        let bwd = self.best[1].get(i).map(|b| b.0);
        let mut modes = Vec::with_capacity(3);
        if let Some(f) = fwd {
            modes.push(Mode::Inter {
                flags: MB_FWD,
                mv: [f, [0, 0]],
            });
        }
        if let Some(b) = bwd {
            modes.push(Mode::Inter {
                flags: MB_BWD,
                mv: [[0, 0], b],
            });
        }
        if let (Some(f), Some(b)) = (fwd, bwd) {
            modes.push(Mode::Inter {
                flags: MB_FWD | MB_BWD,
                mv: [f, b],
            });
        }
        modes
    }

    fn sad_cost(
        &self,
        src: &[u8; 384],
        mode: Mode,
        mbx: usize,
        mby: usize,
        state: &SliceState,
    ) -> u32 {
        match mode {
            Mode::Intra => intra_sad(src) + INTRA_BIAS,
            Mode::Inter { flags, mv } => {
                let pred = self.predict(mbx, mby, flags, &mv);
                let sad: u32 = src[..256]
                    .iter()
                    .zip(&pred[..256])
                    .map(|(&a, &b)| a.abs_diff(b) as u32)
                    .sum();
                let mut bits = 0;
                for s in 0..2 {
                    if flags & [MB_FWD, MB_BWD][s] != 0 {
                        bits += mv_bits(mv[s][0] - state.pmv[s][0])
                            + mv_bits(mv[s][1] - state.pmv[s][1]);
                    }
                }
                sad + self.lambda_sad * bits
            }
        }
    }

    fn decide(
        &self,
        src: &[u8; 384],
        mbx: usize,
        mby: usize,
        state: &SliceState,
        can_skip: bool,
    ) -> (Coded, bool) {
        if self.kind == PictureType::I {
            return (self.evaluate(src, Mode::Intra, mbx, mby), false);
        }
        let skip_mode = state
            .skip_mode(self.kind)
            .filter(|mode| can_skip && self.usable(*mode, mbx, mby));
        let mut modes = self.inter_modes(mbx, mby);
        if self.kind == PictureType::P {
            let zero = Mode::Inter {
                flags: MB_FWD,
                mv: [[0; 2]; 2],
            };
            if !modes.contains(&zero) {
                modes.push(zero);
            }
        }
        modes.push(Mode::Intra);
        let mut best: Option<(f32, Coded)> = None;
        if self.hq {
            if modes.len() > RD_MODES {
                let mut ranked: Vec<(u32, Mode)> = modes
                    .iter()
                    .map(|&m| (self.sad_cost(src, m, mbx, mby, state), m))
                    .collect();
                ranked.sort_by_key(|r| r.0);
                modes = ranked.into_iter().take(RD_MODES).map(|r| r.1).collect();
            }
            for mode in modes {
                let coded = self.evaluate(src, mode, mbx, mby);
                let skippable = Some(mode) == skip_mode && coded.cbp == 0;
                let bits = if skippable {
                    0
                } else {
                    self.bits(state, &coded)
                };
                let cost = coded.sse as f32 + self.lambda * bits as f32;
                if best.as_ref().is_none_or(|b| cost < b.0) {
                    best = Some((cost, coded));
                }
            }
            if let Some(mode) = skip_mode {
                let Mode::Inter { flags, mv } = mode else {
                    unreachable!()
                };
                let pred = self.predict(mbx, mby, flags, &mv);
                let cost = sse(src, &pred) as f32;
                if best.as_ref().is_none_or(|b| cost < b.0) {
                    let skipped = Coded {
                        mode,
                        levels: [[0; 64]; 6],
                        cbp: 0,
                        recon: pred,
                        sse: cost as u64,
                    };
                    return (skipped, true);
                }
            }
        } else {
            let mode = modes
                .into_iter()
                .min_by_key(|&m| self.sad_cost(src, m, mbx, mby, state))
                .expect("at least intra");
            best = Some((0.0, self.evaluate(src, mode, mbx, mby)));
        }
        let (_, coded) = best.expect("a mode was evaluated");
        let skip = Some(coded.mode) == skip_mode && coded.cbp == 0;
        (coded, skip)
    }

    fn code_mb(&self, s: &mut impl Sink, state: &mut SliceState, coded: &Coded, increment: u32) {
        vlc::put_increment(s, increment);
        let kind = self.kind as u8;
        match coded.mode {
            Mode::Intra => {
                vlc::put_mb_type(s, kind, MB_INTRA);
                state.pmv = [[0; 2]; 2];
                let table = vlc::dct(true);
                for (b, levels) in coded.levels.iter().enumerate() {
                    let cc = b.saturating_sub(3);
                    let dc = levels[0] as i32;
                    vlc::put_dc(s, dc - state.dc[cc], cc > 0);
                    state.dc[cc] = dc;
                    put_ac(s, levels, 1, table, false);
                }
                state.prev = None;
            }
            Mode::Inter { flags, mv } => {
                let pattern = coded.cbp != 0;
                let mut type_flags = flags | if pattern { MB_PATTERN } else { 0 };
                if self.kind == PictureType::P && mv[0] == [0, 0] && pattern {
                    type_flags = MB_PATTERN;
                }
                vlc::put_mb_type(s, kind, type_flags);
                for (sdir, dir) in [MB_FWD, MB_BWD].into_iter().enumerate() {
                    if type_flags & dir == 0 {
                        continue;
                    }
                    for (t, (&v, &p)) in mv[sdir].iter().zip(&state.pmv[sdir]).enumerate() {
                        let r = self.r_size[sdir][t];
                        vlc::put_motion(s, wrap(v - p, r), r);
                    }
                    state.pmv[sdir] = mv[sdir];
                }
                if self.kind == PictureType::P && type_flags & MB_FWD == 0 {
                    state.pmv = [[0; 2]; 2];
                }
                if pattern {
                    vlc::put_cbp(s, coded.cbp);
                    let table = vlc::dct(false);
                    for (b, levels) in coded.levels.iter().enumerate() {
                        if coded.cbp & (32 >> b) != 0 {
                            put_ac(s, levels, 0, table, true);
                        }
                    }
                }
                state.dc = [128; 3];
                state.prev = Some((flags, mv));
            }
        }
    }
}

fn put_ac(s: &mut impl Sink, levels: &[i16; 64], start: usize, table: &vlc::DctTable, inter: bool) {
    let mut run = 0;
    let mut first = inter;
    for &j in &ZIGZAG[start..] {
        let level = levels[j] as i32;
        if level == 0 {
            run += 1;
            continue;
        }
        table.put(s, run, level, first);
        first = false;
        run = 0;
    }
    table.put_eob(s);
}

fn sse(a: &[u8; 384], b: &[u8; 384]) -> u64 {
    let sum: u32 = a
        .iter()
        .zip(b)
        .map(|(&x, &y)| {
            let d = x.abs_diff(y) as u32;
            d * d
        })
        .sum();
    sum as u64
}

fn intra_sad(src: &[u8; 384]) -> u32 {
    (0..4)
        .map(|b| {
            let px: [u32; 64] = BLOCKS[b].map(|k| src[k as usize] as u32);
            let mean = (px.iter().sum::<u32>() + 32) / 64;
            px.iter().map(|&p| p.abs_diff(mean)).sum::<u32>()
        })
        .sum()
}
