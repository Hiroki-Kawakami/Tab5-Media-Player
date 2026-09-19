// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};

use anyhow::{Context, Result, bail};

pub trait Source {
    fn size(&self) -> u64;
    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> Result<()>;
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
