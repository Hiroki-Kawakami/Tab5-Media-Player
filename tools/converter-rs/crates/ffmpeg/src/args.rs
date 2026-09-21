// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use tab5conv_core::audio::{AudioAction, AudioPlan, Codec};
use tab5conv_core::video::{Color, Picture, VideoCodec, VideoPlan, h264, mpeg2};

const X264_PARAMS: &str = "bframes=3:b-pyramid=none:ref=1:weightp=0";
const X264_BASELINE_PARAMS: &str = "ref=1";

pub fn encoders(video: &VideoPlan, audio: &AudioPlan) -> Vec<&'static str> {
    let video = match video.codec {
        VideoCodec::H264(_) => Some("libx264"),
        VideoCodec::Mpeg2(_) => Some("mpeg2video"),
        VideoCodec::Mjpeg(_) => None,
    };
    let audio = match audio.action {
        AudioAction::Encode { codec, .. } => Some(audio_encoder(codec)),
        AudioAction::None | AudioAction::Copy { .. } => None,
    };
    video.into_iter().chain(audio).collect()
}

fn transpose(degrees: u32) -> &'static str {
    match degrees {
        90 => "transpose=cclock",
        270 => "transpose=clock",
        180 => "hflip,vflip",
        other => unreachable!("rotation of {other} degrees"),
    }
}

pub fn filter(picture: &Picture) -> String {
    let resize = &picture.resize;
    let color = match picture.color {
        Color::Source => "",
        Color::Bt601Full => ":out_color_matrix=bt601:out_range=full",
    };
    let mut parts: Vec<String> = picture
        .convert_rate
        .map(|rate| format!("fps={rate}"))
        .into_iter()
        .collect();
    parts.push(format!(
        "scale={}:{}{color},setsar=1",
        resize.scaled_width, resize.scaled_height
    ));
    if (resize.scaled_width, resize.scaled_height) != (resize.width, resize.height) {
        parts.push(format!("crop={}:{}", resize.width, resize.height));
    }
    if let Some(rotation) = picture.rotation {
        parts.push(transpose(rotation.degrees).into());
    }
    parts.join(",")
}

/// The filter for the one frame the cover art is made of: the output's own
/// crop, in the orientation the player shows (so the rotation is only applied
/// when it is not handed to the player as metadata), scaled to the thumbnail
/// and always converted to BT.601 full range, which is what a JPEG is read as.
pub fn thumbnail_filter(picture: &Picture, (width, height): (u32, u32)) -> String {
    let resize = &picture.resize;
    let mut parts = vec![format!(
        "scale={}:{}",
        resize.scaled_width, resize.scaled_height
    )];
    if (resize.scaled_width, resize.scaled_height) != (resize.width, resize.height) {
        parts.push(format!("crop={}:{}", resize.width, resize.height));
    }
    if let Some(rotation) = picture.rotation.filter(|r| r.display_rotation.is_none()) {
        parts.push(transpose(rotation.degrees).into());
    }
    parts.push(format!(
        "scale={width}:{height}:out_color_matrix=bt601:out_range=full,setsar=1"
    ));
    parts.join(",")
}

fn strings<const N: usize>(items: [&str; N]) -> impl Iterator<Item = String> {
    items.into_iter().map(String::from)
}

pub fn h264(picture: &Picture, p: &h264::Params) -> Vec<String> {
    let mut args = vec!["-filter:v:0".to_string(), filter(picture)];
    args.extend(strings([
        "-c:v:0",
        "libx264",
        "-profile:v:0",
        p.profile.name(),
    ]));
    args.extend(strings([
        "-preset:v:0",
        p.preset,
        "-pix_fmt:v:0",
        "yuv420p",
    ]));
    args.extend(strings([
        "-x264-params:v:0",
        if p.profile == h264::Profile::Baseline {
            X264_BASELINE_PARAMS
        } else {
            X264_PARAMS
        },
    ]));
    args.extend(match p.rate {
        h264::Rate::Crf(crf) => ["-crf:v:0".into(), crf.to_string()],
        h264::Rate::Bitrate(bitrate) => ["-b:v:0".into(), bitrate.to_string()],
    });
    args.extend(["-g:v:0".into(), p.keyint.to_string()]);
    args
}

pub fn mpeg2(picture: &Picture, p: &mpeg2::Params) -> Vec<String> {
    let mut args = vec!["-filter:v:0".to_string(), filter(picture)];
    args.extend(strings(["-c:v:0", "mpeg2video", "-pix_fmt:v:0", "yuv420p"]));
    args.extend(match p.rate {
        mpeg2::Rate::Qscale(q) => ["-q:v:0".into(), q.to_string()],
        mpeg2::Rate::Bitrate(bitrate) => ["-b:v:0".into(), bitrate.to_string()],
    });
    args.extend([
        "-bf:v:0".into(),
        p.bframes.to_string(),
        "-g:v:0".into(),
        p.keyint.to_string(),
    ]);
    if p.closed_gop {
        args.extend(strings([
            "-flags:v:0",
            "+cgop",
            "-sc_threshold:v:0",
            "1000000000",
        ]));
    }
    if p.hq {
        args.extend(strings([
            "-mbd:v:0",
            "rd",
            "-trellis:v:0",
            "1",
            "-intra_vlc:v:0",
            "1",
        ]));
    }
    args
}

fn audio_encoder(codec: Codec) -> &'static str {
    match codec {
        Codec::Aac { .. } => "aac",
        Codec::Mp3Cbr { .. } | Codec::Mp3Vbr { .. } => "libmp3lame",
    }
}

pub fn audio(plan: &AudioPlan) -> Vec<String> {
    match plan.action {
        AudioAction::None => Vec::new(),
        AudioAction::Copy { .. } => strings(["-c:a", "copy"]).collect(),
        AudioAction::Encode {
            codec,
            channels,
            sample_rate,
            ..
        } => {
            let (key, value) = match codec {
                Codec::Aac { bitrate } | Codec::Mp3Cbr { bitrate } => ("-b:a", bitrate),
                Codec::Mp3Vbr { quality } => ("-q:a", quality.into()),
            };
            [
                "-c:a",
                audio_encoder(codec),
                key,
                &value.to_string(),
                "-ac",
                &channels.to_string(),
                "-ar",
                &sample_rate.to_string(),
            ]
            .map(String::from)
            .to_vec()
        }
    }
}

#[cfg(test)]
mod tests {
    use tab5conv_core::audio;
    use tab5conv_core::framerate::Rate;
    use tab5conv_core::media;
    use tab5conv_core::spec::Spec;
    use tab5conv_core::video;

    use super::*;

    fn source() -> media::Video {
        media::Video {
            index: 0,
            display_width: 1920.0,
            display_height: 1080.0,
            fps: Rate::new(30000, 1001),
        }
    }

    fn plan_for(text: &str, source: &media::Video) -> VideoPlan {
        video::from_spec(Spec::parse(text).unwrap())
            .unwrap()
            .plan(source)
            .unwrap()
    }

    fn args(text: &str) -> Vec<String> {
        let plan = plan_for(text, &source());
        match &plan.codec {
            VideoCodec::H264(p) => h264(&plan.picture, p),
            VideoCodec::Mpeg2(p) => mpeg2(&plan.picture, p),
            VideoCodec::Mjpeg(_) => panic!("not an ffmpeg encode"),
        }
    }

    fn vf(text: &str) -> String {
        filter(&plan_for(text, &source()).picture)
    }

    fn has(args: &[String], pair: [&str; 2]) -> bool {
        args.windows(2).any(|w| w[0] == pair[0] && w[1] == pair[1])
    }

    #[test]
    fn filters() {
        let fast = media::Video {
            fps: Rate::new(60, 1),
            ..source()
        };
        let vf_fast = |text| filter(&plan_for(text, &fast).picture);
        assert_eq!(vf_fast("h264"), "fps=30,scale=640:360,setsar=1");
        assert_eq!(vf_fast("mpeg2,fps=24"), "fps=24,scale=640:360,setsar=1");
        assert_eq!(vf_fast("h264,maxfps=60"), "scale=640:360,setsar=1");
        assert_eq!(
            vf("h264,width=720,height=720,scale=cover"),
            "scale=1280:720,setsar=1,crop=720:720"
        );
        assert_eq!(
            vf("mjpeg"),
            "fps=30000/1001,scale=1280:720:out_color_matrix=bt601:out_range=full,setsar=1,transpose=cclock"
        );
        assert!(vf("mjpeg,maxfps=24,long=640,rotate=0").starts_with("fps=24,scale=640:360:"));
        assert!(vf("mjpeg,rotatewhen=always,rotate=-90").ends_with(",transpose=clock"));
        assert!(vf("mjpeg,rotate=180").ends_with(",hflip,vflip"));
    }

    #[test]
    fn h264_args() {
        let a = args("h264");
        assert!(has(&a, ["-filter:v:0", "scale=640:360,setsar=1"]));
        assert!(has(&a, ["-c:v:0", "libx264"]));
        assert!(has(&a, ["-profile:v:0", "main"]));
        assert!(has(&a, ["-preset:v:0", "medium"]));
        assert!(has(&a, ["-crf:v:0", "32"]));
        assert!(has(
            &a,
            [
                "-x264-params:v:0",
                "bframes=3:b-pyramid=none:ref=1:weightp=0"
            ]
        ));
        assert!(has(&a, ["-g:v:0", "120"]));
        let a = args("h264,profile=baseline");
        assert!(has(&a, ["-x264-params:v:0", "ref=1"]));
        let a = args("h264,profile=high,bitrate=1.5M,keyint=0.5,preset=fast");
        assert!(has(&a, ["-profile:v:0", "high"]));
        assert!(has(&a, ["-b:v:0", "1500000"]));
        assert!(!a.contains(&"-crf:v:0".to_string()));
        assert!(has(&a, ["-g:v:0", "15"]));
        assert!(has(&a, ["-preset:v:0", "fast"]));
    }

    #[test]
    fn mpeg2_args() {
        let a = args("mpeg2");
        assert!(has(&a, ["-c:v:0", "mpeg2video"]));
        assert!(has(&a, ["-pix_fmt:v:0", "yuv420p"]));
        assert!(has(&a, ["-q:v:0", "8"]));
        assert!(has(&a, ["-bf:v:0", "2"]));
        assert!(has(&a, ["-g:v:0", "60"]));
        assert!(has(&a, ["-flags:v:0", "+cgop"]));
        assert!(has(&a, ["-sc_threshold:v:0", "1000000000"]));
        assert!(has(&a, ["-mbd:v:0", "rd"]));
        assert!(has(&a, ["-trellis:v:0", "1"]));
        assert!(has(&a, ["-intra_vlc:v:0", "1"]));
        let a = args("mpeg2,bitrate=2M,bframes=0,keyint=1,gop=open,hq=no,short=720");
        assert!(has(&a, ["-filter:v:0", "scale=1280:720,setsar=1"]));
        assert!(has(&a, ["-b:v:0", "2000000"]));
        assert!(!a.contains(&"-q:v:0".to_string()));
        assert!(has(&a, ["-bf:v:0", "0"]));
        assert!(!a.contains(&"+cgop".to_string()));
        assert!(!a.contains(&"-sc_threshold:v:0".to_string()));
        assert!(!a.contains(&"-mbd:v:0".to_string()));
    }

    #[test]
    fn thumbnail_filters() {
        // mjpeg stores the picture turned with the rotation in the metadata,
        // so the cover is made without the transpose.
        let p = plan_for("mjpeg,long=640,short=360", &source()).picture;
        assert_eq!(
            thumbnail_filter(&p, (320, 180)),
            "scale=640:360,scale=320:180:out_color_matrix=bt601:out_range=full,setsar=1"
        );
        let p = plan_for("mjpeg,long=640,short=360,rotatemeta=no", &source()).picture;
        assert_eq!(
            thumbnail_filter(&p, (180, 320)),
            "scale=640:360,transpose=cclock,scale=180:320:out_color_matrix=bt601:out_range=full,setsar=1"
        );
        let p = plan_for("h264,width=720,height=720,scale=cover", &source()).picture;
        assert_eq!(
            thumbnail_filter(&p, (320, 320)),
            "scale=1280:720,crop=720:720,scale=320:320:out_color_matrix=bt601:out_range=full,setsar=1"
        );
    }

    #[test]
    fn audio_args_and_encoders() {
        let input = media::Audio {
            index: 1,
            codec_name: "aac".into(),
            profile: Some("LC".into()),
            channels: 2,
            sample_rate: 48000,
            bit_rate: Some(128_000),
        };
        let plan = |text: &str| {
            audio::from_spec(Spec::parse(text).unwrap())
                .unwrap()
                .plan(Some(&input))
                .unwrap()
        };
        let mjpeg = plan_for("mjpeg", &source());
        let h264 = plan_for("h264", &source());
        let p = plan("aac");
        assert_eq!(audio(&p), ["-c:a", "copy"]);
        assert!(encoders(&mjpeg, &p).is_empty());
        let p = plan("aac,keep=none,channels=1,samplerate=44.1k");
        assert_eq!(
            audio(&p),
            ["-c:a", "aac", "-b:a", "160000", "-ac", "1", "-ar", "44100"]
        );
        assert_eq!(encoders(&h264, &p), ["libx264", "aac"]);
        let p = plan("mp3");
        assert_eq!(
            audio(&p),
            [
                "-c:a",
                "libmp3lame",
                "-b:a",
                "192000",
                "-ac",
                "2",
                "-ar",
                "48000"
            ]
        );
        assert_eq!(encoders(&mjpeg, &p), ["libmp3lame"]);
        let p = plan("mp3,vbr=0,samplerate=22.05k");
        assert_eq!(
            audio(&p),
            [
                "-c:a",
                "libmp3lame",
                "-q:a",
                "0",
                "-ac",
                "2",
                "-ar",
                "22050"
            ]
        );
        assert!(audio(&plan("none")).is_empty());
    }
}
