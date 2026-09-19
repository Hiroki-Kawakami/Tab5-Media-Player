// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::HashMap;

use anyhow::{Context, anyhow, bail};
use js_sys::{Function, Uint8Array};
use serde::Serialize;
use tab5conv_core::audio::{AudioAction, Codec as AudioCodec};
use tab5conv_core::container::demux::{self, Codec, Demuxer, TrackKind};
use tab5conv_core::container::interleave::{Interleaver, Sample};
use tab5conv_core::container::io::{Sink, Source};
use tab5conv_core::container::mux::{Mp4Muxer, MuxCodec, MuxKind, MuxTrack};
use tab5conv_core::framerate::{FrameSelector, Rate};
use tab5conv_core::jpeg::{self, Coefficients, Encoded, Frame, HuffmanMode, Limits, SizeModel};
use tab5conv_core::media::MediaInfo;
use tab5conv_core::mjpeg::{Reorder, Scheduler, Tally};
use tab5conv_core::ratecontrol::Decision;
use tab5conv_core::video::mjpeg::Settings as MjpegSettings;
use tab5conv_core::video::{Color, VideoCodec, VideoPlan};
use tab5conv_core::{Specs, audio, color, pcm, preset, video};
use wasm_bindgen::prelude::*;

const VIDEO_TRACK: usize = 0;
const AUDIO_TRACK: usize = 1;
const VIDEO_TIMESCALE: u32 = 1_200_000;
const MAX_INTERLEAVE_BYTES: usize = 256 << 20;

fn js_error(err: anyhow::Error) -> JsError {
    JsError::new(&format!("{err:#}"))
}

fn json(value: &impl Serialize) -> String {
    serde_json::to_string(value).expect("serialisable")
}

#[derive(Serialize)]
struct PresetInfo {
    name: &'static str,
    video: &'static str,
    audio: &'static str,
}

#[wasm_bindgen]
pub fn presets() -> String {
    json(
        &preset::all()
            .iter()
            .map(|p| PresetInfo {
                name: p.name,
                video: p.video,
                audio: p.audio,
            })
            .collect::<Vec<_>>(),
    )
}

#[wasm_bindgen]
pub fn help() -> String {
    json(&serde_json::json!({
        "preset": preset::help(),
        "video": video::HELP,
        "audio": audio::HELP,
    }))
}

#[wasm_bindgen]
pub fn resolve(preset: &str, video: &str, audio: &str) -> Result<String, JsError> {
    let specs = resolve_specs(preset, video, audio).map_err(js_error)?;
    Ok(json(&applied(&specs)))
}

fn optional(text: &str) -> Option<&str> {
    Some(text.trim()).filter(|t| !t.is_empty())
}

fn resolve_specs(preset: &str, video: &str, audio: &str) -> anyhow::Result<Specs> {
    Specs::resolve(preset, optional(video), optional(audio))
}

fn applied(specs: &Specs) -> serde_json::Value {
    serde_json::json!({
        "preset": specs.preset,
        "video": specs.video_text,
        "audio": specs.audio_text,
    })
}

struct JsSource {
    size: u64,
    read: Function,
}

impl Source for JsSource {
    fn size(&self) -> u64 {
        self.size
    }

    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> anyhow::Result<()> {
        let value = self
            .read
            .call2(
                &JsValue::NULL,
                &JsValue::from_f64(offset as f64),
                &JsValue::from_f64(buf.len() as f64),
            )
            .map_err(|e| anyhow!("reading the input failed: {e:?}"))?;
        let array = Uint8Array::new(&value);
        if array.length() as usize != buf.len() {
            bail!("short read from the input");
        }
        array.copy_to(buf);
        Ok(())
    }
}

struct JsSink {
    write: Function,
    write_at: Function,
}

impl Sink for JsSink {
    fn write(&mut self, data: &[u8]) -> anyhow::Result<()> {
        self.write
            .call1(&JsValue::NULL, &Uint8Array::from(data))
            .map_err(|e| anyhow!("writing the output failed: {e:?}"))?;
        Ok(())
    }

    fn write_at(&mut self, offset: u64, data: &[u8]) -> anyhow::Result<()> {
        self.write_at
            .call2(
                &JsValue::NULL,
                &JsValue::from_f64(offset as f64),
                &Uint8Array::from(data),
            )
            .map_err(|e| anyhow!("writing the output failed: {e:?}"))?;
        Ok(())
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct TrackInfo {
    index: u32,
    kind: &'static str,
    codec: &'static str,
    codec_string: Option<String>,
    width: u32,
    height: u32,
    rotation: u32,
    color: Option<[u8; 3]>,
    full_range: Option<bool>,
    channels: u32,
    sample_rate: u32,
}

#[wasm_bindgen]
pub struct Packet {
    track: u32,
    data: Vec<u8>,
    pts: i64,
    duration: i64,
    key: bool,
}

#[wasm_bindgen]
impl Packet {
    #[wasm_bindgen(getter)]
    pub fn track(&self) -> u32 {
        self.track
    }

    #[wasm_bindgen(getter)]
    pub fn data(&self) -> Vec<u8> {
        self.data.clone()
    }

    #[wasm_bindgen(getter)]
    pub fn pts(&self) -> f64 {
        self.pts as f64
    }

    #[wasm_bindgen(getter)]
    pub fn duration(&self) -> f64 {
        self.duration as f64
    }

    #[wasm_bindgen(getter)]
    pub fn key(&self) -> bool {
        self.key
    }
}

#[wasm_bindgen]
pub struct Media {
    demuxer: Demuxer<JsSource>,
    info: MediaInfo,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PictureInfo {
    scaled_width: u32,
    scaled_height: u32,
    width: u32,
    height: u32,
    stored_width: u32,
    stored_height: u32,
    rotation: u32,
    rate_num: u64,
    rate_den: u64,
    bt601_full: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AudioInfo {
    label: String,
    action: &'static str,
    index: Option<u32>,
    codec: Option<&'static str>,
    bitrate: Option<u64>,
    channels: Option<u32>,
    sample_rate: Option<u32>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PlanInfo {
    applied: serde_json::Value,
    duration: Option<f64>,
    video_label: String,
    video_index: u32,
    video_codec: &'static str,
    picture: PictureInfo,
    audio: AudioInfo,
}

struct Planned {
    specs: Specs,
    video: VideoPlan,
    audio: tab5conv_core::audio::AudioPlan,
}

fn plan(
    info: &MediaInfo,
    preset: &str,
    video: &str,
    audio: &str,
    aac_rates: &[u32],
) -> anyhow::Result<Planned> {
    let specs = resolve_specs(preset, video, audio)?;
    let video = specs.video.plan(&info.video)?;
    let audio = specs.audio.plan_for(info.audio.as_ref(), aac_rates)?;
    Ok(Planned {
        specs,
        video,
        audio,
    })
}

fn plan_info(info: &MediaInfo, planned: &Planned) -> PlanInfo {
    let p = &planned.video.picture;
    let source = &info.video;
    let audio = &planned.audio;
    let (action, codec, bitrate, channels, sample_rate) = match audio.action {
        AudioAction::None => ("none", None, None, None, None),
        AudioAction::Copy { .. } => ("copy", None, None, None, None),
        AudioAction::Encode {
            codec,
            channels,
            sample_rate,
            ..
        } => {
            let (name, bitrate) = match codec {
                AudioCodec::Aac { bitrate } => ("aac", Some(bitrate)),
                AudioCodec::Mp3Cbr { bitrate } => ("mp3", Some(bitrate)),
                AudioCodec::Mp3Vbr { .. } => ("mp3", None),
            };
            (
                "encode",
                Some(name),
                bitrate,
                Some(channels),
                Some(sample_rate),
            )
        }
    };
    PlanInfo {
        applied: applied(&planned.specs),
        duration: info.duration,
        video_label: format!(
            "{}x{} -> {}",
            source.display_width.round(),
            source.display_height.round(),
            planned.video.label
        ),
        video_index: planned.video.index,
        video_codec: match planned.video.codec {
            VideoCodec::H264(_) => "h264",
            VideoCodec::Mpeg2(_) => "mpeg2",
            VideoCodec::Mjpeg(_) => "mjpeg",
        },
        picture: PictureInfo {
            scaled_width: p.resize.scaled_width,
            scaled_height: p.resize.scaled_height,
            width: p.resize.width,
            height: p.resize.height,
            stored_width: p.width,
            stored_height: p.height,
            rotation: p.rotation.map_or(0, |r| r.degrees),
            rate_num: p.rate.num(),
            rate_den: p.rate.den(),
            bt601_full: p.color == Color::Bt601Full,
        },
        audio: AudioInfo {
            label: audio.label.clone(),
            action,
            index: audio.index(),
            codec,
            bitrate,
            channels,
            sample_rate,
        },
    }
}

#[wasm_bindgen]
impl Media {
    #[wasm_bindgen(constructor)]
    pub fn new(size: f64, read: Function) -> Result<Media, JsError> {
        let source = JsSource {
            size: size as u64,
            read,
        };
        let demuxer = demux::open(source).map_err(js_error)?;
        let info = demuxer.info().media_info().map_err(js_error)?;
        Ok(Self { demuxer, info })
    }

    pub fn tracks(&self) -> String {
        let tracks: Vec<TrackInfo> = self
            .demuxer
            .info()
            .tracks
            .iter()
            .map(|t| {
                let mut info = TrackInfo {
                    index: t.index,
                    kind: "other",
                    codec: t.codec.name(),
                    codec_string: t.codec_string(),
                    width: 0,
                    height: 0,
                    rotation: 0,
                    color: None,
                    full_range: None,
                    channels: 0,
                    sample_rate: 0,
                };
                match &t.kind {
                    TrackKind::Video(v) => {
                        info.kind = "video";
                        (info.width, info.height, info.rotation) = (v.width, v.height, v.rotation);
                        info.color = v.color.map(|c| [c.primaries, c.transfer, c.matrix]);
                        info.full_range = v.color.and_then(|c| c.full_range);
                    }
                    TrackKind::Audio(a) => {
                        info.kind = "audio";
                        (info.channels, info.sample_rate) = (a.channels, a.sample_rate);
                    }
                    TrackKind::Other => {}
                }
                info
            })
            .collect();
        json(&tracks)
    }

    pub fn extradata(&self, track: u32) -> Vec<u8> {
        self.demuxer
            .info()
            .tracks
            .get(track as usize)
            .map(|t| t.extradata.clone())
            .unwrap_or_default()
    }

    pub fn plan(
        &self,
        preset: &str,
        video: &str,
        audio: &str,
        aac_rates: Vec<u32>,
    ) -> Result<String, JsError> {
        let planned = plan(&self.info, preset, video, audio, &aac_rates).map_err(js_error)?;
        Ok(json(&plan_info(&self.info, &planned)))
    }

    pub fn select(&mut self, tracks: Vec<u32>) {
        self.demuxer.select(&tracks);
    }

    #[wasm_bindgen(js_name = nextPacket)]
    pub fn next_packet(&mut self) -> Result<Option<Packet>, JsError> {
        Ok(self
            .demuxer
            .next_packet()
            .map_err(js_error)?
            .map(|p| Packet {
                track: p.track,
                data: p.data,
                pts: p.pts,
                duration: p.duration,
                key: p.key,
            }))
    }
}

#[wasm_bindgen]
pub struct EncodedFrame {
    data: Vec<u8>,
    quality: u8,
    fits: bool,
}

#[wasm_bindgen]
impl EncodedFrame {
    #[wasm_bindgen(getter)]
    pub fn data(&self) -> Vec<u8> {
        self.data.clone()
    }

    #[wasm_bindgen(getter)]
    pub fn quality(&self) -> u8 {
        self.quality
    }

    #[wasm_bindgen(getter)]
    pub fn fits(&self) -> bool {
        self.fits
    }
}

#[wasm_bindgen]
#[derive(Default)]
pub struct MjpegWorker {
    frames: HashMap<u32, Coefficients>,
    yuv: Vec<u8>,
}

#[wasm_bindgen]
impl MjpegWorker {
    #[wasm_bindgen(constructor)]
    pub fn new() -> MjpegWorker {
        Self::default()
    }

    pub fn analyze(&mut self, id: u32, rgba: &[u8], width: usize, height: usize) -> Vec<u32> {
        self.yuv.resize(Frame::bytes(width, height), 0);
        color::rgba_to_yuv420_bt601_full(rgba, width, height, &mut self.yuv);
        self.analyze_buffer(id, width, height)
    }

    #[wasm_bindgen(js_name = analyzeYuv)]
    pub fn analyze_yuv(&mut self, id: u32, yuv: &[u8], width: usize, height: usize) -> Vec<u32> {
        self.yuv.clear();
        self.yuv.extend_from_slice(yuv);
        self.analyze_buffer(id, width, height)
    }

    fn analyze_buffer(&mut self, id: u32, width: usize, height: usize) -> Vec<u32> {
        let frame = Frame::from_yuv420p(&self.yuv, width, height);
        let (coefficients, model) = jpeg::analyze(&frame);
        self.frames.insert(id, coefficients);
        model.to_words()
    }

    pub fn encode(
        &mut self,
        id: u32,
        quality: u8,
        min_quality: u8,
        max_frame: usize,
        optimal: bool,
    ) -> Result<EncodedFrame, JsError> {
        let coefficients = self
            .frames
            .remove(&id)
            .ok_or_else(|| JsError::new(&format!("frame {id} was not analysed")))?;
        let limits = Limits {
            min_quality,
            max_frame,
            huffman: if optimal {
                HuffmanMode::Optimal
            } else {
                HuffmanMode::Standard
            },
        };
        let encoded = jpeg::encode(&coefficients, quality, &limits);
        Ok(EncodedFrame {
            data: encoded.data,
            quality: encoded.quality,
            fits: encoded.fits,
        })
    }
}

#[wasm_bindgen]
pub struct DecisionOut {
    pub index: u32,
    pub quality: u8,
}

#[wasm_bindgen]
pub struct Job {
    muxer: Mp4Muxer<JsSink>,
    scheduler: Scheduler,
    tally: Tally,
    settings: MjpegSettings,
    rate: Rate,
    selector: FrameSelector,
    decisions: HashMap<usize, Decision>,
    order: Reorder<Encoded>,
    interleaver: Interleaver,
    has_audio: bool,
    audio_rate: u32,
    audio_started: bool,
    encoded: u64,
}

fn mux_tracks(info: &demux::Info, planned: &Planned) -> anyhow::Result<Vec<MuxTrack>> {
    let p = &planned.video.picture;
    let mut tracks = vec![MuxTrack {
        codec: MuxCodec::Mjpeg,
        kind: MuxKind::Video {
            width: p.width,
            height: p.height,
            display_rotation: p.rotation.and_then(|r| r.display_rotation),
        },
        timescale: VIDEO_TIMESCALE,
    }];
    match planned.audio.action {
        AudioAction::None => {}
        AudioAction::Copy { index } => {
            let track = info
                .tracks
                .get(index as usize)
                .context("no such audio track")?;
            let TrackKind::Audio(a) = &track.kind else {
                bail!("track {index} is not audio");
            };
            tracks.push(MuxTrack {
                codec: match track.codec {
                    Codec::Aac => MuxCodec::Aac {
                        config: track.extradata.clone(),
                    },
                    _ => MuxCodec::Mp3,
                },
                kind: MuxKind::Audio {
                    channels: a.channels,
                    sample_rate: a.sample_rate,
                },
                timescale: a.sample_rate,
            });
        }
        AudioAction::Encode {
            codec,
            channels,
            sample_rate,
            ..
        } => {
            if !matches!(codec, AudioCodec::Aac { .. }) {
                bail!(
                    "MP3 encoding is not available in the browser; use aac or keep an MP3 input as is"
                );
            }
            tracks.push(MuxTrack {
                codec: MuxCodec::Aac { config: Vec::new() },
                kind: MuxKind::Audio {
                    channels,
                    sample_rate,
                },
                timescale: sample_rate,
            });
        }
    }
    Ok(tracks)
}

#[wasm_bindgen]
impl Job {
    #[wasm_bindgen(constructor)]
    pub fn new(
        media: &Media,
        preset: &str,
        video: &str,
        audio: &str,
        aac_rates: Vec<u32>,
        write: Function,
        write_at: Function,
    ) -> Result<Job, JsError> {
        let planned = plan(&media.info, preset, video, audio, &aac_rates).map_err(js_error)?;
        let VideoCodec::Mjpeg(settings) = planned.video.codec else {
            return Err(JsError::new(
                "only MJPEG can be encoded in the browser for now",
            ));
        };
        let tracks = mux_tracks(media.demuxer.info(), &planned).map_err(js_error)?;
        let has_audio = tracks.len() > 1;
        let audio_rate = tracks.get(AUDIO_TRACK).map_or(0, |t| t.timescale);
        let rate = planned.video.picture.rate;
        let fps = rate.as_f64();
        let muxer = Mp4Muxer::new(JsSink { write, write_at }, tracks).map_err(js_error)?;
        Ok(Self {
            muxer,
            scheduler: Scheduler::new(&settings, fps),
            tally: Tally::new(&settings, fps),
            settings,
            rate,
            selector: FrameSelector::new(rate),
            decisions: HashMap::new(),
            order: Reorder::default(),
            interleaver: Interleaver::new(if has_audio { 2 } else { 1 }, MAX_INTERLEAVE_BYTES),
            has_audio,
            audio_rate,
            audio_started: false,
            encoded: 0,
        })
    }

    pub fn limits(&self) -> Vec<u32> {
        vec![
            self.settings.min_quality as u32,
            self.settings.max_frame as u32,
            u32::from(self.settings.huffman == HuffmanMode::Optimal),
        ]
    }

    pub fn repeats(&mut self, pts_us: f64) -> u32 {
        self.selector.push(pts_us as i64) as u32
    }

    #[wasm_bindgen(js_name = finishFrames)]
    pub fn finish_frames(&mut self, end_us: f64) -> u32 {
        self.selector.finish(end_us as i64) as u32
    }

    #[wasm_bindgen(js_name = pushModel)]
    pub fn push_model(&mut self, index: u32, words: &[u32]) -> Result<(), JsError> {
        let model = SizeModel::from_words(words).ok_or_else(|| JsError::new("bad size model"))?;
        self.scheduler.push(index as usize, model);
        Ok(())
    }

    #[wasm_bindgen(js_name = nextDecision)]
    pub fn next_decision(&mut self, finished: bool) -> Option<DecisionOut> {
        let decided = self.scheduler.decide(finished)?;
        self.decisions.insert(decided.index, decided.decision);
        Some(DecisionOut {
            index: decided.index as u32,
            quality: decided.decision.quality,
        })
    }

    #[wasm_bindgen(js_name = addFrame)]
    pub fn add_frame(
        &mut self,
        index: u32,
        data: Vec<u8>,
        quality: u8,
        fits: bool,
    ) -> Result<u32, JsError> {
        self.order.push(
            index as usize,
            Encoded {
                data,
                quality,
                fits,
            },
        );
        while let Some(encoded) = self.order.pop() {
            let index = self.encoded as usize;
            let decision = self
                .decisions
                .remove(&index)
                .ok_or_else(|| JsError::new(&format!("frame {index} has no decision")))?;
            let feedback = self
                .tally
                .record(index, &encoded, &decision)
                .map_err(js_error)?;
            self.scheduler.feedback(&feedback);
            let (start, end) = (
                self.video_ticks(self.encoded),
                self.video_ticks(self.encoded + 1),
            );
            self.interleaver.push(
                VIDEO_TRACK,
                Sample {
                    time: (start as i128 * 1_000_000 / VIDEO_TIMESCALE as i128) as i64,
                    data: encoded.data,
                    duration: (end - start) as u32,
                    key: true,
                },
            );
            self.encoded += 1;
        }
        self.write_ready().map_err(js_error)?;
        Ok(self.encoded as u32)
    }

    fn video_ticks(&self, frame: u64) -> u64 {
        let ticks = frame as u128 * VIDEO_TIMESCALE as u128 * self.rate.den() as u128;
        let num = self.rate.num() as u128;
        ((ticks * 2 + num) / (num * 2)) as u64
    }

    fn write_ready(&mut self) -> anyhow::Result<()> {
        while let Some((track, sample)) = self.interleaver.pop() {
            self.muxer
                .write(track, &sample.data, sample.duration, sample.key)?;
        }
        Ok(())
    }

    #[wasm_bindgen(js_name = addAudio)]
    pub fn add_audio(&mut self, data: Vec<u8>, pts_us: f64, duration: u32) -> Result<(), JsError> {
        if !self.has_audio {
            return Ok(());
        }
        if !self.audio_started {
            self.audio_started = true;
            if pts_us < 0.0 {
                let skip = (-pts_us * self.audio_rate as f64 / 1e6).round() as u32;
                self.muxer.set_skip(AUDIO_TRACK, skip).map_err(js_error)?;
            }
        }
        self.interleaver.push(
            AUDIO_TRACK,
            Sample {
                time: pts_us as i64,
                data,
                duration,
                key: true,
            },
        );
        self.write_ready().map_err(js_error)
    }

    #[wasm_bindgen(js_name = setAudioConfig)]
    pub fn set_audio_config(&mut self, config: Vec<u8>) -> Result<(), JsError> {
        self.muxer
            .set_codec(AUDIO_TRACK, MuxCodec::Aac { config })
            .map_err(js_error)
    }

    pub fn finish(mut self) -> Result<String, JsError> {
        self.interleaver.close_all();
        self.write_ready().map_err(js_error)?;
        self.muxer.finish().map_err(js_error)?;
        Ok(json(
            &self
                .tally
                .finish()
                .report(&self.settings, self.rate.as_f64()),
        ))
    }
}

#[wasm_bindgen]
pub struct AudioConverter {
    input_channels: usize,
    output_channels: usize,
    resampler: Option<pcm::Resampler>,
}

fn planes(samples: &[f32], channels: usize) -> Vec<Vec<f32>> {
    let frames = samples.len() / channels.max(1);
    (0..channels)
        .map(|c| samples[c * frames..(c + 1) * frames].to_vec())
        .collect()
}

fn flatten(planes: Vec<Vec<f32>>) -> Vec<f32> {
    planes.concat()
}

#[wasm_bindgen]
impl AudioConverter {
    #[wasm_bindgen(constructor)]
    pub fn new(
        input_rate: u32,
        input_channels: usize,
        output_rate: u32,
        output_channels: usize,
    ) -> AudioConverter {
        Self {
            input_channels,
            output_channels,
            resampler: (input_rate != output_rate)
                .then(|| pcm::Resampler::new(input_rate, output_rate, output_channels)),
        }
    }

    pub fn process(&mut self, planar: &[f32]) -> Vec<f32> {
        let mixed = pcm::downmix(&planes(planar, self.input_channels), self.output_channels);
        match &mut self.resampler {
            Some(resampler) => flatten(resampler.process(&mixed)),
            None => flatten(mixed),
        }
    }

    pub fn flush(&mut self) -> Vec<f32> {
        match &mut self.resampler {
            Some(resampler) => flatten(resampler.flush()),
            None => Vec::new(),
        }
    }
}
