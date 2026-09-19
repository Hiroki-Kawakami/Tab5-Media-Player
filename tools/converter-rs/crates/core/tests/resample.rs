// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::process::Command;

use tab5conv_core::resample::Resampler;

fn ffmpeg(args: &[&str]) -> Vec<u8> {
    let output = Command::new("ffmpeg")
        .args(["-hide_banner", "-loglevel", "error", "-y"])
        .args(args)
        .output()
        .expect("ffmpeg must be in PATH");
    assert!(output.status.success(), "ffmpeg {args:?}");
    output.stdout
}

fn psnr(a: &[u8], b: &[u8]) -> f64 {
    let (mut sum, mut n) = (0f64, 0f64);
    for (p, q) in a.chunks(4).zip(b.chunks(4)) {
        for c in 0..3 {
            let d = p[c] as f64 - q[c] as f64;
            sum += d * d;
            n += 1.0;
        }
    }
    10.0 * (255.0 * 255.0 / (sum / n)).log10()
}

#[test]
fn close_to_ffmpeg_bicubic() {
    for (source, (sw, sh), (dw, dh), floor) in [
        ("testsrc2", (1920, 1080), (640, 360), 45.0),
        ("mandelbrot", (1920, 1080), (640, 360), 45.0),
        ("testsrc2", (1080, 1920), (360, 640), 45.0),
        ("mandelbrot", (2560, 1440), (854, 480), 45.0),
    ] {
        let input = format!("{source}=s={sw}x{sh}");
        let raw = |filter: &str| {
            ffmpeg(&[
                "-f",
                "lavfi",
                "-i",
                &input,
                "-frames:v",
                "1",
                "-vf",
                filter,
                "-f",
                "rawvideo",
                "-pix_fmt",
                "rgba",
                "-",
            ])
        };
        let src = raw("format=rgba");
        let reference = raw(&format!("format=rgba,scale={dw}:{dh}"));
        let mut out = vec![0; dw * dh * 4];
        Resampler::new(sw, sh, dw, dh).rgba(&src, &mut out);
        let value = psnr(&out, &reference);
        assert!(
            value > floor,
            "{source} {sw}x{sh} -> {dw}x{dh}: {value:.1} dB"
        );
        eprintln!("{source} {sw}x{sh} -> {dw}x{dh}: {value:.1} dB");
    }
}

fn psnr_plane(a: &[u8], b: &[u8]) -> f64 {
    let sum: f64 = a
        .iter()
        .zip(b)
        .map(|(&p, &q)| (p as f64 - q as f64).powi(2))
        .sum();
    10.0 * (255.0 * 255.0 / (sum / a.len() as f64)).log10()
}

#[test]
fn yuv_geometry_matches_ffmpeg_filters() {
    use tab5conv_core::color::Matrix;
    use tab5conv_core::yuv::{Converter, Geometry, Layout, Output, Source, SourceColor};
    let cases = [
        ("1920x1080", (640, 360), (640, 360), 0, 0, "scale=640:360"),
        (
            "1920x1080",
            (720, 405),
            (720, 404),
            0,
            0,
            "scale=720:405,crop=720:404",
        ),
        (
            "1920x1080",
            (1280, 720),
            (720, 720),
            0,
            90,
            "scale=1280:720,crop=720:720,transpose=cclock",
        ),
        (
            "1080x1920",
            (640, 360),
            (640, 360),
            90,
            0,
            "transpose=clock,scale=640:360",
        ),
        (
            "1920x1080",
            (640, 360),
            (640, 360),
            180,
            270,
            "hflip,vflip,scale=640:360,transpose=clock",
        ),
    ];
    for (size, scaled, crop, source_rotation, output_rotation, filter) in cases {
        let (sw, sh): (usize, usize) = {
            let (a, b) = size.split_once('x').unwrap();
            (a.parse().unwrap(), b.parse().unwrap())
        };
        let input = format!("mandelbrot=s={size}");
        let raw = |f: &str| {
            ffmpeg(&[
                "-f",
                "lavfi",
                "-i",
                &input,
                "-frames:v",
                "1",
                "-vf",
                f,
                "-f",
                "rawvideo",
                "-pix_fmt",
                "yuv420p",
                "-",
            ])
        };
        let src = raw("format=yuv420p");
        let reference = raw(&format!("format=yuv420p,{filter}"));
        let stored = if output_rotation % 180 == 90 {
            (crop.1, crop.0)
        } else {
            crop
        };
        let geometry = Geometry {
            scaled,
            crop,
            source_rotation,
            output_rotation,
            stored,
            output: Output::Limited,
        };
        let color = SourceColor {
            matrix: Matrix::Bt601,
            full_range: false,
        };
        let luma = sw * sh;
        let source = Source {
            layout: Layout::I420,
            width: sw,
            height: sh,
            data: &src,
            planes: &[(0, sw), (luma, sw / 2), (luma + luma / 4, sw / 2)],
        };
        let out = Converter::new(geometry).convert(&source, color).unwrap();
        assert_eq!(out.len(), reference.len(), "{filter}");
        let n = stored.0 * stored.1;
        let (y, c) = (
            psnr_plane(&out[..n], &reference[..n]),
            psnr_plane(&out[n..], &reference[n..]),
        );
        eprintln!("{filter}: luma {y:.1} dB, chroma {c:.1} dB");
        assert!(
            y > 40.0 && c > 40.0,
            "{filter}: luma {y:.1} dB, chroma {c:.1} dB"
        );
    }
}

#[test]
fn colour_conversion_matches_ffmpeg() {
    use tab5conv_core::color::Matrix;
    use tab5conv_core::yuv::{Converter, Geometry, Layout, Output, Source, SourceColor};
    let (sw, sh) = (1280, 720);
    let input = format!("testsrc2=s={sw}x{sh}");
    for (matrix, name) in [(Matrix::Bt709, "bt709"), (Matrix::Bt601, "bt601")] {
        let raw = |f: &str| {
            ffmpeg(&[
                "-f",
                "lavfi",
                "-i",
                &input,
                "-frames:v",
                "1",
                "-vf",
                f,
                "-f",
                "rawvideo",
                "-pix_fmt",
                "yuv420p",
                "-",
            ])
        };
        let src = raw(&format!(
            "format=rgb24,scale=out_color_matrix={name}:out_range=tv,format=yuv420p"
        ));
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("src.yuv");
        std::fs::write(&path, &src).unwrap();
        let reference = ffmpeg(&[
            "-f",
            "rawvideo",
            "-pix_fmt",
            "yuv420p",
            "-s",
            &format!("{sw}x{sh}"),
            "-i",
            path.to_str().unwrap(),
            "-vf",
            &format!(
                "scale=in_color_matrix={name}:in_range=tv:out_color_matrix=bt601:out_range=full,format=yuv420p"
            ),
            "-f",
            "rawvideo",
            "-",
        ]);
        let geometry = Geometry {
            scaled: (sw, sh),
            crop: (sw, sh),
            source_rotation: 0,
            output_rotation: 0,
            stored: (sw, sh),
            output: Output::Bt601Full,
        };
        let luma = sw * sh;
        let source = Source {
            layout: Layout::I420,
            width: sw,
            height: sh,
            data: &src,
            planes: &[(0, sw), (luma, sw / 2), (luma + luma / 4, sw / 2)],
        };
        let color = SourceColor {
            matrix,
            full_range: false,
        };
        let out = Converter::new(geometry).convert(&source, color).unwrap();
        let (y, c) = (
            psnr_plane(&out[..luma], &reference[..luma]),
            psnr_plane(&out[luma..], &reference[luma..]),
        );
        eprintln!("{name} -> bt601 full: luma {y:.1} dB, chroma {c:.1} dB");
        assert!(
            y > 40.0 && c > 40.0,
            "{name}: luma {y:.1} dB, chroma {c:.1} dB"
        );
    }
}
