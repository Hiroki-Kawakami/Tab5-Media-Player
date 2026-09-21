// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};

use anyhow::{Context, Result, bail};

const STREAM_CHUNK: usize = 64 << 10;
const STREAM_LOOKBACK: usize = 64 << 10;

pub trait Source {
    fn size(&self) -> u64;
    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> Result<()>;

    fn read_upto(&mut self, offset: u64, buf: &mut [u8]) -> Result<usize> {
        self.read_at(offset, buf)?;
        Ok(buf.len())
    }
}

pub trait Sink {
    fn write(&mut self, data: &[u8]) -> Result<()>;
    fn write_at(&mut self, offset: u64, data: &[u8]) -> Result<()>;
}

impl Source for &[u8] {
    fn size(&self) -> u64 {
        <[u8]>::len(self) as u64
    }

    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> Result<()> {
        let start = usize::try_from(offset).context("offset out of range")?;
        match self.get(start..start + buf.len()) {
            Some(data) => {
                buf.copy_from_slice(data);
                Ok(())
            }
            None => bail!("read past the end of the input"),
        }
    }
}

impl Source for File {
    fn size(&self) -> u64 {
        self.metadata().map_or(0, |m| m.len())
    }

    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> Result<()> {
        self.seek(SeekFrom::Start(offset))?;
        self.read_exact(buf)
            .context("read past the end of the input")
    }

    fn read_upto(&mut self, offset: u64, buf: &mut [u8]) -> Result<usize> {
        self.seek(SeekFrom::Start(offset))?;
        let mut filled = 0;
        while filled < buf.len() {
            match self.read(&mut buf[filled..])? {
                0 => break,
                n => filled += n,
            }
        }
        Ok(filled)
    }
}

pub struct Stream<R> {
    reader: R,
    buf: Vec<u8>,
    start: u64,
    eof: bool,
}

impl<R: Read> Stream<R> {
    pub fn new(reader: R) -> Self {
        Self {
            reader,
            buf: Vec::new(),
            start: 0,
            eof: false,
        }
    }

    fn fill(&mut self, need: usize) -> Result<()> {
        while self.buf.len() < need && !self.eof {
            let filled = self.buf.len();
            let want = (need - filled).max(STREAM_CHUNK);
            self.buf.resize(filled + want, 0);
            let read = self.reader.read(&mut self.buf[filled..])?;
            self.buf.truncate(filled + read);
            self.eof = read == 0;
        }
        Ok(())
    }
}

impl<R: Read> Source for Stream<R> {
    fn size(&self) -> u64 {
        u64::MAX
    }

    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> Result<()> {
        if self.read_upto(offset, buf)? < buf.len() {
            bail!("read past the end of the input");
        }
        Ok(())
    }

    fn read_upto(&mut self, offset: u64, buf: &mut [u8]) -> Result<usize> {
        if offset < self.start {
            bail!("a stream cannot be read backwards");
        }
        let start = usize::try_from(offset - self.start).context("offset out of range")?;
        self.fill(start + buf.len())?;
        let available = self.buf.len().saturating_sub(start).min(buf.len());
        buf[..available].copy_from_slice(&self.buf[start..start + available]);
        if let Some(drop) = start.checked_sub(STREAM_LOOKBACK).filter(|d| *d > 0) {
            self.buf.drain(..drop);
            self.start += drop as u64;
        }
        Ok(available)
    }
}

impl Sink for Vec<u8> {
    fn write(&mut self, data: &[u8]) -> Result<()> {
        self.extend_from_slice(data);
        Ok(())
    }

    fn write_at(&mut self, offset: u64, data: &[u8]) -> Result<()> {
        let start = usize::try_from(offset).context("offset out of range")?;
        match self.get_mut(start..start + data.len()) {
            Some(target) => {
                target.copy_from_slice(data);
                Ok(())
            }
            None => bail!("write past the end of the output"),
        }
    }
}

impl Sink for File {
    fn write(&mut self, data: &[u8]) -> Result<()> {
        self.write_all(data)?;
        Ok(())
    }

    fn write_at(&mut self, offset: u64, data: &[u8]) -> Result<()> {
        let end = self.stream_position()?;
        self.seek(SeekFrom::Start(offset))?;
        self.write_all(data)?;
        self.seek(SeekFrom::Start(end))?;
        Ok(())
    }
}
