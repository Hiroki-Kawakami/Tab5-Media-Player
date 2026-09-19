// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::sync::OnceLock;

use super::bits::Sink;
use super::tables::*;

pub const MB_FWD: u8 = 2;
pub const MB_BWD: u8 = 4;
pub const MB_PATTERN: u8 = 8;
pub const MB_INTRA: u8 = 16;

const MAX_LEVEL: usize = 40;
const RUNS: usize = 32;

pub struct DctTable {
    codes: [[(u16, u8); MAX_LEVEL + 1]; RUNS],
    pub eob: (u16, u8),
}

fn build(table: &[Code]) -> DctTable {
    let mut t = DctTable {
        codes: [[(0, 0); MAX_LEVEL + 1]; RUNS],
        eob: (0, 0),
    };
    for &(code, len, value) in table {
        match value {
            DCT_EOB => t.eob = (code, len),
            DCT_ESCAPE => {}
            _ => t.codes[(value & 31) as usize][(value >> 5) as usize] = (code, len),
        }
    }
    t
}

pub fn dct(intra_vlc: bool) -> &'static DctTable {
    static TABLES: OnceLock<[DctTable; 2]> = OnceLock::new();
    &TABLES.get_or_init(|| [build(&DCT0), build(&DCT1)])[usize::from(intra_vlc)]
}

impl DctTable {
    pub fn bits(&self, run: usize, level: i32, first_inter: bool) -> u32 {
        let mag = level.unsigned_abs() as usize;
        if first_inter && run == 0 && mag == 1 {
            return 2;
        }
        match self.codes.get(run).and_then(|r| r.get(mag)) {
            Some(&(_, len)) if len > 0 => len as u32 + 1,
            _ => 24,
        }
    }

    pub fn put(&self, s: &mut impl Sink, run: usize, level: i32, first_inter: bool) {
        let mag = level.unsigned_abs() as usize;
        let sign = u32::from(level < 0);
        if first_inter && run == 0 && mag == 1 {
            s.put(2 | sign, 2);
            return;
        }
        match self.codes.get(run).and_then(|r| r.get(mag)) {
            Some(&(code, len)) if len > 0 => s.put(((code as u32) << 1) | sign, len as u32 + 1),
            _ => {
                s.put(1, 6);
                s.put(run as u32, 6);
                s.put((level as u32) & 0xFFF, 12);
            }
        }
    }

    pub fn put_eob(&self, s: &mut impl Sink) {
        s.put(self.eob.0 as u32, self.eob.1 as u32);
    }
}

fn find(table: &[Code], value: i16) -> (u32, u32) {
    let &(code, len, _) = table
        .iter()
        .find(|c| c.2 == value)
        .unwrap_or_else(|| panic!("no code for {value}"));
    (code as u32, len as u32)
}

pub fn put_increment(s: &mut impl Sink, mut increment: u32) {
    while increment > 33 {
        s.put(MBAI_ESCAPE.0 as u32, MBAI_ESCAPE.1 as u32);
        increment -= 33;
    }
    let (code, len, _) = MBAI[increment as usize - 1];
    s.put(code as u32, len as u32);
}

pub fn put_mb_type(s: &mut impl Sink, picture: u8, flags: u8) {
    let table: &[Code] = match picture {
        1 => &MB_TYPE_I,
        2 => &MB_TYPE_P,
        _ => &MB_TYPE_B,
    };
    let (code, len) = find(table, flags as i16);
    s.put(code, len);
}

pub fn put_cbp(s: &mut impl Sink, cbp: u8) {
    let (code, len) = find(&CBP, cbp as i16);
    s.put(code, len);
}

pub fn put_motion(s: &mut impl Sink, delta: i32, r_size: u32) {
    if delta == 0 {
        s.put(1, 1);
        return;
    }
    let mag = delta.unsigned_abs() - 1;
    let code = (mag >> r_size) as i32 + 1;
    let (c, len) = find(&MOTION, if delta < 0 { -code } else { code } as i16);
    s.put(c, len);
    s.put(mag & ((1 << r_size) - 1), r_size);
}

pub fn put_dc(s: &mut impl Sink, diff: i32, chroma: bool) {
    let size = 32 - diff.unsigned_abs().leading_zeros();
    let table = if chroma { &DC_CHROMA } else { &DC_LUMA };
    let (code, len, _) = table[size as usize];
    s.put(code as u32, len as u32);
    if size > 0 {
        let value = if diff > 0 {
            diff
        } else {
            diff + (1 << size) - 1
        };
        s.put(value as u32, size);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mpeg2::bits::BitWriter;

    #[test]
    fn tables_are_indexed_by_value() {
        for (i, c) in MBAI.iter().enumerate() {
            assert_eq!(c.2 as usize, i + 1);
        }
        for v in -16..=16 {
            assert_eq!(MOTION.iter().filter(|c| c.2 == v).count(), 1);
        }
        for (i, c) in DC_LUMA.iter().chain(&DC_CHROMA).enumerate() {
            assert_eq!(c.2 as usize, i % 12);
        }
    }

    #[test]
    fn dct_codes() {
        let t = dct(false);
        assert_eq!(t.eob, (0b10, 2));
        assert_eq!(t.bits(0, 1, true), 2);
        assert_eq!(t.bits(0, -1, false), 3);
        assert_eq!(t.bits(1, 1, false), 4);
        assert_eq!(t.bits(31, 1, false), 17);
        assert_eq!(t.bits(0, 41, false), 24);
        assert_eq!(dct(true).eob, (0b0110, 4));
        let mut w = BitWriter::default();
        t.put(&mut w, 0, -1, true);
        t.put(&mut w, 1, 1, false);
        t.put(&mut w, 2, -300, false);
        assert_eq!(w.finish(), [0xD8, 0x10, 0xBB, 0x50]);
    }
}
