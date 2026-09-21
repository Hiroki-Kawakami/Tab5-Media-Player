// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use anyhow::{Context, Result, bail};

use crate::jpeg::{self, Frame, HuffmanMode, Limits};
use crate::spec::{Spec, int_in, positive_int};
use crate::video::Picture;
use crate::yuv::{Converter, Geometry, Layout, Output, Source, SourceColor};

pub const KEYS: [&str; 3] = ["at", "long", "quality"];
pub const DEFAULT: &str = "jpeg";

pub const HELP: &str = "\
--thumbnail <kind>[,key=value]...

jpeg   a picture from the video, stored as cover art so the file browser can
       show it (default); MP4 keeps it in moov/udta/meta/ilst/covr, MKV as an
       attachment named cover.jpg
  at=S                seconds into the video (default 10), lowered to half the
                      duration when the video is shorter; not the first frame,
                      which is often still black
  long=N              long side in pixels (default 320), following the output's
                      aspect ratio and orientation; never upscaled
  quality=N           JPEG quality 1-100 (default 85)
none   no cover art
";

const DEFAULT_AT: f64 = 10.0;
const DEFAULT_LONG: u32 = 320;
const DEFAULT_QUALITY: u32 = 85;

const LIMITS: Limits = Limits {
    min_quality: 1,
    max_frame: usize::MAX,
    huffman: HuffmanMode::Optimal,
};

#[derive(Debug)]
pub enum ThumbnailSpec {
    Jpeg { at: f64, long: u32, quality: u8 },
    None,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Thumbnail {
    pub at: f64,
    pub width: u32,
    pub height: u32,
    pub quality: u8,
}

fn at_seconds(value: &str) -> Result<f64> {
    let v: f64 = value.parse().context("not a number")?;
    if !(v.is_finite() && v >= 0.0) {
        bail!("must be 0 or more");
    }
    Ok(v)
}

#[cfg(test)]
pub fn parse(text: &str) -> Result<ThumbnailSpec> {
    from_spec(Spec::parse(text)?)
}

pub fn from_spec(mut spec: Spec) -> Result<ThumbnailSpec> {
    let (thumbnail, keys): (_, &[&str]) = match spec.codec.clone().as_str() {
        "jpeg" => {
            let at = spec.take("at", at_seconds)?.unwrap_or(DEFAULT_AT);
            let long = spec.take("long", positive_int)?.unwrap_or(DEFAULT_LONG);
            let quality = spec
                .take("quality", |v| int_in(v, 1, 100))?
                .unwrap_or(DEFAULT_QUALITY) as u8;
            (ThumbnailSpec::Jpeg { at, long, quality }, &KEYS)
        }
        "none" => (ThumbnailSpec::None, &[]),
        other => bail!("unknown thumbnail kind '{other}' (kinds: jpeg, none)"),
    };
    spec.finish(keys)?;
    Ok(thumbnail)
}

/// The size the player shows, which is the stored size turned back by the
/// rotation metadata. Cover art carries no rotation of its own.
fn displayed(picture: &Picture) -> (u32, u32) {
    match picture.rotation {
        Some(r) if r.display_rotation.is_some() => (picture.resize.width, picture.resize.height),
        _ => (picture.width, picture.height),
    }
}

fn even(value: u32) -> u32 {
    value.max(2) & !1
}

fn fit(width: u32, height: u32, long: u32) -> (u32, u32) {
    let source = width.max(height);
    if source == 0 || source <= long {
        return (even(width), even(height));
    }
    let short = (width.min(height) as f64 * long as f64 / source as f64).round() as u32;
    if width >= height {
        (even(long), even(short))
    } else {
        (even(short), even(long))
    }
}

impl ThumbnailSpec {
    pub fn plan(&self, picture: &Picture, duration: Option<f64>) -> Option<Thumbnail> {
        let Self::Jpeg { at, long, quality } = self else {
            return None;
        };
        let (shown_width, shown_height) = displayed(picture);
        let (width, height) = fit(shown_width, shown_height, *long);
        let at = match duration {
            Some(seconds) if seconds > 0.0 => at.min(seconds / 2.0),
            _ => *at,
        };
        Some(Thumbnail {
            at,
            width,
            height,
            quality: *quality,
        })
    }

    pub fn label(&self, thumbnail: Option<&Thumbnail>) -> String {
        match thumbnail {
            Some(t) => format!(
                "{}x{} at {:.1}s, quality {}",
                t.width, t.height, t.at, t.quality
            ),
            None => "none".into(),
        }
    }
}

/// Turns an I420 picture that is already the thumbnail's size and in BT.601
/// full range into the JPEG the container stores.
pub fn encode(width: u32, height: u32, i420: &[u8], quality: u8) -> Result<Vec<u8>> {
    let (w, h) = (width as usize, height as usize);
    let needed = Frame::bytes(w, h);
    if i420.len() < needed {
        bail!("thumbnail frame is {} bytes, expected {needed}", i420.len());
    }
    let (coefficients, _) = jpeg::analyze(&Frame::from_yuv420p(i420, w, h));
    Ok(jpeg::encode(&coefficients, quality, &LIMITS).data)
}

/// Makes the cover out of a stored picture, which a front end that scales the
/// video itself already has. The stored picture is turned back into the
/// displayed orientation and converted to BT.601 full range, since nothing
/// carries either of those alongside a JPEG.
pub struct Cover {
    thumbnail: Thumbnail,
    stored: (usize, usize),
    geometry: Geometry,
}

pub fn geometry(picture: &Picture, thumbnail: &Thumbnail) -> Geometry {
    let turn = match picture.rotation {
        Some(r) if r.display_rotation.is_some() => (360 - r.degrees % 360) % 360,
        _ => 0,
    };
    let stored = (thumbnail.width as usize, thumbnail.height as usize);
    let scaled = if turn % 180 == 90 {
        (stored.1, stored.0)
    } else {
        stored
    };
    Geometry {
        scaled,
        crop: scaled,
        source_rotation: 0,
        output_rotation: turn,
        stored,
        output: Output::Bt601Full,
    }
}

impl Cover {
    pub fn new(picture: &Picture, thumbnail: Thumbnail) -> Self {
        Self {
            thumbnail,
            stored: (picture.width as usize, picture.height as usize),
            geometry: geometry(picture, &thumbnail),
        }
    }

    pub fn from_i420(&self, data: &[u8], color: SourceColor) -> Result<Vec<u8>> {
        let (w, h) = self.stored;
        let (cw, ch) = (w.div_ceil(2), h.div_ceil(2));
        let planes = [(0, w), (w * h, cw), (w * h + cw * ch, cw)];
        let source = Source {
            layout: Layout::I420,
            width: w,
            height: h,
            data,
            planes: &planes,
        };
        let scaled = Converter::new(self.geometry).convert(&source, color)?;
        encode(
            self.thumbnail.width,
            self.thumbnail.height,
            &scaled,
            self.thumbnail.quality,
        )
    }
}

#[cfg(test)]
mod tests {
    use crate::framerate::Rate;
    use crate::media;
    use crate::video;

    use super::*;

    fn picture(text: &str, width: f64, height: f64) -> Picture {
        video::parse(text)
            .unwrap()
            .plan(&media::Video {
                index: 0,
                display_width: width,
                display_height: height,
                fps: Rate::new(30, 1),
            })
            .unwrap()
            .picture
    }

    fn plan(text: &str, picture: &Picture, duration: Option<f64>) -> Option<Thumbnail> {
        parse(text).unwrap().plan(picture, duration)
    }

    #[test]
    fn defaults_are_320_long_at_ten_seconds() {
        let p = picture("h264", 1920.0, 1080.0);
        let t = plan("jpeg", &p, Some(60.0)).unwrap();
        assert_eq!((t.width, t.height), (320, 180));
        assert_eq!((t.at, t.quality), (10.0, 85));
    }

    #[test]
    fn follows_the_displayed_orientation() {
        // mjpeg turns a landscape picture 90 degrees and says so in the
        // metadata, so the cover is the unrotated 320x180.
        let p = picture("mjpeg,long=640,short=360", 1920.0, 1080.0);
        assert_eq!((p.width, p.height), (360, 640));
        let t = plan("jpeg", &p, None).unwrap();
        assert_eq!((t.width, t.height), (320, 180));
        assert_eq!(geometry(&p, &t).output_rotation, 270);

        // Without the metadata the player shows the stored picture as it is.
        let p = picture("mjpeg,long=640,short=360,rotatemeta=no", 1920.0, 1080.0);
        let t = plan("jpeg", &p, None).unwrap();
        assert_eq!((t.width, t.height), (180, 320));
        assert_eq!(geometry(&p, &t).output_rotation, 0);

        let p = picture("h264,long=640,short=360", 1080.0, 1920.0);
        let t = plan("jpeg", &p, None).unwrap();
        assert_eq!((t.width, t.height), (180, 320));
    }

    #[test]
    fn never_upscales_and_keeps_sizes_even() {
        let p = picture("h264,width=240,height=178", 480.0, 356.0);
        let t = plan("jpeg", &p, None).unwrap();
        assert_eq!((t.width, t.height), (240, 178));
        let t = plan("jpeg,long=100", &p, None).unwrap();
        assert_eq!((t.width, t.height), (100, 74));
    }

    #[test]
    fn short_videos_take_a_frame_from_the_middle() {
        let p = picture("h264", 1920.0, 1080.0);
        assert_eq!(plan("jpeg", &p, Some(4.0)).unwrap().at, 2.0);
        assert_eq!(plan("jpeg", &p, Some(600.0)).unwrap().at, 10.0);
        assert_eq!(plan("jpeg", &p, None).unwrap().at, 10.0);
        assert_eq!(plan("jpeg,at=0", &p, Some(600.0)).unwrap().at, 0.0);
    }

    #[test]
    fn none_has_no_thumbnail() {
        let p = picture("h264", 1920.0, 1080.0);
        assert!(plan("none", &p, Some(60.0)).is_none());
    }

    #[test]
    fn rejects_bad_specs() {
        assert!(parse("jpeg,quality=0").is_err());
        assert!(parse("jpeg,quality=101").is_err());
        assert!(parse("jpeg,at=-1").is_err());
        assert!(parse("jpeg,long=0").is_err());
        assert!(parse("none,at=3").is_err());
        let err = parse("png").unwrap_err().to_string();
        assert!(err.contains("jpeg, none"), "{err}");
        let err = parse("jpeg,when=3").unwrap_err().to_string();
        assert!(err.contains("at, long, quality"), "{err}");
    }

    #[test]
    fn encodes_a_jpeg() {
        let p = picture("mjpeg,long=64,short=36", 640.0, 360.0);
        let t = plan("jpeg,long=32", &p, None).unwrap();
        assert_eq!((t.width, t.height), (32, 18));
        let stored = vec![128u8; Frame::bytes(p.width as usize, p.height as usize)];
        let color = SourceColor {
            matrix: crate::color::Matrix::Bt601,
            full_range: true,
        };
        let jpeg = Cover::new(&p, t).from_i420(&stored, color).unwrap();
        assert_eq!(&jpeg[..2], &[0xFF, 0xD8]);
        assert_eq!(&jpeg[jpeg.len() - 2..], &[0xFF, 0xD9]);
    }
}
