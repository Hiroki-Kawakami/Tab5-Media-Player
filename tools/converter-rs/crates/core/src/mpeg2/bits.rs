// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

pub trait Sink {
    fn put(&mut self, value: u32, len: u32);
}

#[derive(Default)]
pub struct Counter(pub u32);

impl Sink for Counter {
    fn put(&mut self, _value: u32, len: u32) {
        self.0 += len;
    }
}

#[derive(Default)]
pub struct BitWriter {
    buf: Vec<u8>,
    acc: u64,
    bits: u32,
}

impl Sink for BitWriter {
    fn put(&mut self, value: u32, len: u32) {
        debug_assert!(len <= 32 && (len == 32 || value >> len == 0));
        if len == 0 {
            return;
        }
        self.acc = (self.acc << len) | value as u64;
        self.bits += len;
        while self.bits >= 8 {
            self.bits -= 8;
            self.buf.push((self.acc >> self.bits) as u8);
        }
    }
}

impl BitWriter {
    pub fn align(&mut self) {
        let pad = (8 - self.bits % 8) % 8;
        self.put(0, pad);
    }

    pub fn start_code(&mut self, code: u8) {
        self.align();
        self.buf.extend([0, 0, 1, code]);
    }

    pub fn finish(mut self) -> Vec<u8> {
        self.align();
        self.buf
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packs_msb_first() {
        let mut w = BitWriter::default();
        w.put(0b101, 3);
        w.put(0x1F, 5);
        w.put(1, 1);
        w.start_code(0xB3);
        w.put(0xABCD, 16);
        assert_eq!(w.finish(), [0xBF, 0x80, 0, 0, 1, 0xB3, 0xAB, 0xCD]);
    }
}
