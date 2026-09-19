// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use super::tables::HuffmanSpec;

#[derive(Clone, Copy, Default)]
pub struct Code {
    pub bits: u16,
    pub len: u8,
}

pub struct Table {
    pub spec: HuffmanSpec,
    pub codes: [Code; 256],
}

impl Table {
    pub fn new(spec: HuffmanSpec) -> Self {
        let mut codes = [Code::default(); 256];
        let mut code = 0u16;
        let mut k = 0;
        for len in 1..=16u8 {
            for _ in 0..spec.bits[len as usize - 1] {
                codes[spec.values[k] as usize] = Code { bits: code, len };
                code += 1;
                k += 1;
            }
            code <<= 1;
        }
        Self { spec, codes }
    }
}

pub fn optimal(freq: &[u32; 256]) -> HuffmanSpec {
    let mut freq: [u64; 257] = std::array::from_fn(|i| if i < 256 { freq[i] as u64 } else { 1 });
    let mut codesize = [0usize; 257];
    let mut others = [usize::MAX; 257];

    loop {
        let mut c1 = None;
        let mut v = u64::MAX;
        for (i, &f) in freq.iter().enumerate() {
            if f != 0 && f <= v {
                v = f;
                c1 = Some(i);
            }
        }
        let Some(mut c1) = c1 else { break };
        let mut c2 = None;
        v = u64::MAX;
        for (i, &f) in freq.iter().enumerate() {
            if f != 0 && f <= v && i != c1 {
                v = f;
                c2 = Some(i);
            }
        }
        let Some(mut c2) = c2 else { break };

        freq[c1] += freq[c2];
        freq[c2] = 0;
        codesize[c1] += 1;
        while others[c1] != usize::MAX {
            c1 = others[c1];
            codesize[c1] += 1;
        }
        others[c1] = c2;
        codesize[c2] += 1;
        while others[c2] != usize::MAX {
            c2 = others[c2];
            codesize[c2] += 1;
        }
    }

    let depth = codesize.iter().copied().max().unwrap_or(0).max(16);
    let mut bits = vec![0u32; depth + 1];
    for &size in &codesize {
        if size > 0 {
            bits[size] += 1;
        }
    }
    for i in (17..=depth).rev() {
        while bits[i] > 0 {
            let mut j = i - 2;
            while bits[j] == 0 {
                j -= 1;
            }
            bits[i] -= 2;
            bits[i - 1] += 1;
            bits[j + 1] += 2;
            bits[j] -= 1;
        }
    }
    let mut i = 16;
    while bits[i] == 0 {
        i -= 1;
    }
    bits[i] -= 1;

    let mut values = Vec::new();
    for size in 1..=depth {
        for (symbol, &s) in codesize.iter().enumerate().take(256) {
            if s == size {
                values.push(symbol as u8);
            }
        }
    }
    HuffmanSpec {
        bits: std::array::from_fn(|i| bits[i + 1] as u8),
        values,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::jpeg::tables::standard_ac_luma;

    fn lengths(spec: &HuffmanSpec) -> Vec<(u8, u8)> {
        let table = Table::new(HuffmanSpec {
            bits: spec.bits,
            values: spec.values.clone(),
        });
        spec.values
            .iter()
            .map(|&v| (v, table.codes[v as usize].len))
            .collect()
    }

    #[test]
    fn standard_codes_are_prefix_free() {
        let table = Table::new(standard_ac_luma());
        let used: Vec<Code> = table
            .spec
            .values
            .iter()
            .map(|&v| table.codes[v as usize])
            .collect();
        for (i, a) in used.iter().enumerate() {
            for b in &used[i + 1..] {
                let (short, long) = if a.len <= b.len { (a, b) } else { (b, a) };
                assert_ne!(long.bits >> (long.len - short.len), short.bits);
            }
        }
    }

    #[test]
    fn optimal_gives_short_codes_to_frequent_symbols() {
        let mut freq = [0u32; 256];
        freq[0] = 1000;
        freq[1] = 100;
        freq[2] = 10;
        freq[3] = 1;
        let spec = optimal(&freq);
        assert_eq!(spec.values.len(), 4);
        let lens = lengths(&spec);
        let len = |s| lens.iter().find(|(v, _)| *v == s).unwrap().1;
        assert!(len(0) <= len(1) && len(1) <= len(2) && len(2) <= len(3));
        assert!(
            Table::new(spec)
                .codes
                .iter()
                .all(|c| c.len == 0 || c.bits != (1 << c.len) - 1)
        );
    }

    #[test]
    fn optimal_limits_code_length_to_16() {
        let mut freq = [0u32; 256];
        let mut f = 1u32;
        for slot in freq.iter_mut().take(40) {
            *slot = f;
            f = f.saturating_mul(2).min(1 << 30);
        }
        let spec = optimal(&freq);
        assert_eq!(spec.values.len(), 40);
        assert_eq!(spec.bits.iter().map(|&b| b as usize).sum::<usize>(), 40);
        let kraft: f64 = spec
            .bits
            .iter()
            .enumerate()
            .map(|(i, &n)| n as f64 / 2f64.powi(i as i32 + 1))
            .sum();
        assert!(kraft < 1.0);
    }

    #[test]
    fn single_symbol() {
        let mut freq = [0u32; 256];
        freq[5] = 42;
        let spec = optimal(&freq);
        assert_eq!(spec.values, vec![5]);
        assert_eq!(spec.bits[0], 1);
    }
}
