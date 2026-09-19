// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

mod bytes;
pub mod demux;
pub mod interleave;
pub mod io;
mod mkv;
mod mp4;
pub mod mux;

use std::path::Path;

use anyhow::{Result, bail};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Container {
    Mp4,
    Mkv,
}

impl Container {
    pub fn from_path(path: &Path) -> Result<Self> {
        let ext = path
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase());
        match ext.as_deref() {
            Some("mp4" | "m4v" | "mov") => Ok(Self::Mp4),
            Some("mkv") => Ok(Self::Mkv),
            _ => bail!(
                "{}: output must be .mp4, .m4v, .mov or .mkv",
                path.display()
            ),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn container_from_extension() {
        assert_eq!(
            Container::from_path(Path::new("a.MP4")).unwrap(),
            Container::Mp4
        );
        assert_eq!(
            Container::from_path(Path::new("a.mkv")).unwrap(),
            Container::Mkv
        );
        assert!(Container::from_path(Path::new("a.avi")).is_err());
        assert!(Container::from_path(Path::new("a")).is_err());
    }
}
