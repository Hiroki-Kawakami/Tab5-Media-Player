// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

import init, { AudioConverter, Job, Media, help, presets, resolve } from "../wasm/tab5conv.js";
import type { Outcome, Plan, PlanItem, Settings } from "../backend";
import { EncoderPool } from "./pool";
import { type Message, type Reply, type Requests, TEMP_DIR } from "./protocol";

interface TrackInfo {
  index: number;
  kind: "video" | "audio" | "other";
  codec: string;
  codecString: string | null;
  width: number;
  height: number;
  rotation: number;
  color: [number, number, number] | null;
  fullRange: boolean | null;
  channels: number;
  sampleRate: number;
}

const PRIMARIES: Record<number, VideoColorPrimaries> = { 1: "bt709", 5: "bt470bg", 6: "smpte170m" };
const TRANSFERS: Record<number, VideoTransferCharacteristics> = { 1: "bt709", 6: "smpte170m", 13: "iec61966-2-1" };
const MATRICES: Record<number, VideoMatrixCoefficients> = { 0: "rgb", 1: "bt709", 5: "bt470bg", 6: "smpte170m" };

function colorSpace(track: TrackInfo): VideoColorSpaceInit | null {
  if (!track.color) return null;
  const [primaries, transfer, matrix] = track.color;
  const init: VideoColorSpaceInit = {
    primaries: PRIMARIES[primaries],
    transfer: TRANSFERS[transfer],
    matrix: MATRICES[matrix],
    fullRange: track.fullRange ?? undefined,
  };
  return init.matrix ? init : null;
}

interface PlanInfo {
  applied: { preset: string; video: string; audio: string };
  duration: number | null;
  videoLabel: string;
  videoIndex: number;
  videoCodec: "mjpeg" | "h264" | "mpeg2";
  picture: {
    scaledWidth: number;
    scaledHeight: number;
    width: number;
    height: number;
    storedWidth: number;
    storedHeight: number;
    rotation: number;
    rateNum: number;
    rateDen: number;
  };
  audio: {
    label: string;
    action: "none" | "copy" | "encode";
    index: number | null;
    codec: "aac" | "mp3" | null;
    bitrate: number | null;
    channels: number | null;
    sampleRate: number | null;
  };
}

class Cancelled extends Error {}

const ready = init();
const reader = new FileReaderSync();
const cancelled = new Set<number>();
const AAC_FRAME = 1024;
const SAMPLE_RATES = [16000, 22050, 24000, 32000, 44100, 48000];
const aacRates: Promise<Uint32Array> = (async () => {
  if (typeof AudioEncoder === "undefined") return new Uint32Array();
  const supported = await Promise.all(
    SAMPLE_RATES.map(async (sampleRate) => {
      const config = { codec: "mp4a.40.2", sampleRate, numberOfChannels: 2, bitrate: 128000 };
      return (await AudioEncoder.isConfigSupported(config).catch(() => ({ supported: false }))).supported;
    }),
  );
  return new Uint32Array(SAMPLE_RATES.filter((_, i) => supported[i]));
})();
const DECODE_QUEUE = 8;
const AUDIO_QUEUE = 16;

function open(file: File): Media {
  return new Media(file.size, (offset: number, length: number) =>
    new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length))),
  );
}

function outputName(name: string): string {
  const dot = name.lastIndexOf(".");
  return `${dot > 0 ? name.slice(0, dot) : name}.tab5.mp4`;
}

function describe(track: TrackInfo): string {
  return track.codec === "unknown" ? "this codec" : track.codec.toUpperCase();
}

async function checkSupport(media: Media, plan: PlanInfo, tracks: TrackInfo[]): Promise<void> {
  if (plan.videoCodec !== "mjpeg") {
    throw new Error("the browser version only writes MJPEG for now; choose the default or quality preset");
  }
  const video = tracks[plan.videoIndex];
  const videoConfig = decoderConfig(media, video);
  if (!videoConfig || !(await VideoDecoder.isConfigSupported(videoConfig)).supported) {
    throw new Error(`this browser cannot decode ${describe(video)} video`);
  }
  const audio = plan.audio;
  if (audio.action !== "encode" || audio.index === null) return;
  if (audio.codec !== "aac") {
    throw new Error("the browser cannot encode MP3; use aac, or keep an MP3 input as it is");
  }
  const source = tracks[audio.index];
  const audioConfig = audioDecoderConfig(media, source);
  if (!audioConfig || !(await AudioDecoder.isConfigSupported(audioConfig)).supported) {
    throw new Error(`this browser cannot decode ${describe(source)} audio`);
  }
  if (!(await AudioEncoder.isConfigSupported(aacConfig(plan))).supported) {
    throw new Error("this browser cannot encode AAC; try Chrome or Edge, or use --audio none");
  }
}

function decoderConfig(media: Media, track: TrackInfo): VideoDecoderConfig | null {
  if (!track.codecString) return null;
  const extradata = media.extradata(track.index);
  const withDescription = ["h264", "hevc", "av1"].includes(track.codec) && extradata.length > 0;
  const color = colorSpace(track);
  return {
    codec: track.codecString,
    codedWidth: track.width,
    codedHeight: track.height,
    ...(withDescription ? { description: extradata } : {}),
    ...(color ? { colorSpace: color } : {}),
  };
}

function audioDecoderConfig(media: Media, track: TrackInfo): AudioDecoderConfig | null {
  if (!track.codecString) return null;
  const extradata = media.extradata(track.index);
  return {
    codec: track.codecString,
    sampleRate: track.sampleRate,
    numberOfChannels: track.channels,
    ...(extradata.length > 0 && track.codec !== "mp3" ? { description: extradata } : {}),
  };
}

function aacConfig(plan: PlanInfo): AudioEncoderConfig {
  return {
    codec: "mp4a.40.2",
    sampleRate: plan.audio.sampleRate!,
    numberOfChannels: plan.audio.channels!,
    bitrate: plan.audio.bitrate!,
  };
}

async function plan(files: File[], keys: string[], settings: Settings): Promise<Plan> {
  const applied: Plan["applied"] = JSON.parse(resolve(settings.preset, settings.video, settings.audio));
  const items: PlanItem[] = [];
  for (const [i, file] of files.entries()) {
    const item: PlanItem = { input: keys[i], output: outputName(file.name), exists: false };
    let media: Media | null = null;
    try {
      media = open(file);
      const info: PlanInfo = JSON.parse(
        media.plan(settings.preset, settings.video, settings.audio, await aacRates),
      );
      await checkSupport(media, info, JSON.parse(media.tracks()));
      Object.assign(item, { video: info.videoLabel, audio: info.audio.label, duration: info.duration });
    } catch (err) {
      item.error = err instanceof Error ? err.message : String(err);
    } finally {
      media?.free();
    }
    items.push(item);
  }
  return { applied, items };
}

async function until(condition: () => boolean, check: () => void): Promise<void> {
  while (!condition()) {
    check();
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
}

function transform(ctx: OffscreenCanvasRenderingContext2D, plan: PlanInfo, sourceRotation: number) {
  const p = plan.picture;
  const [w, h] = [p.width, p.height];
  switch (p.rotation) {
    case 90:
      ctx.setTransform(0, -1, 1, 0, 0, w);
      break;
    case 270:
      ctx.setTransform(0, 1, -1, 0, h, 0);
      break;
    case 180:
      ctx.setTransform(-1, 0, 0, -1, w, h);
      break;
    default:
      ctx.setTransform(1, 0, 0, 1, 0, 0);
  }
  const [sw, sh] = [p.scaledWidth, p.scaledHeight];
  ctx.transform(1, 0, 0, 1, -(sw - w) / 2, -(sh - h) / 2);
  switch (sourceRotation) {
    case 90:
      ctx.transform(0, 1, -1, 0, sw, 0);
      return [sh, sw];
    case 270:
      ctx.transform(0, -1, 1, 0, 0, sh);
      return [sh, sw];
    case 180:
      ctx.transform(-1, 0, 0, -1, sw, sh);
      return [sw, sh];
    default:
      return [sw, sh];
  }
}

async function convert(
  id: number,
  file: File,
  settings: Settings,
  temp: string,
  progress: (fraction: number) => void,
): Promise<Outcome> {
  const check = () => {
    if (cancelled.has(id)) throw new Cancelled();
  };
  const media = open(file);
  const rates = await aacRates;
  const info: PlanInfo = JSON.parse(media.plan(settings.preset, settings.video, settings.audio, rates));
  const tracks: TrackInfo[] = JSON.parse(media.tracks());
  await checkSupport(media, info, tracks);
  const videoTrack = tracks[info.videoIndex];
  const audioIndex = info.audio.action === "none" ? null : info.audio.index;
  media.select(new Uint32Array(audioIndex === null ? [info.videoIndex] : [info.videoIndex, audioIndex]));

  const root = await (await navigator.storage.getDirectory()).getDirectoryHandle(TEMP_DIR, { create: true });
  const handle = await root.getFileHandle(temp, { create: true });
  const access = await handle.createSyncAccessHandle();
  access.truncate(0);
  let position = 0;
  const job = new Job(
    media,
    settings.preset,
    settings.video,
    settings.audio,
    rates,
    (data: Uint8Array) => {
      access.write(data, { at: position });
      position += data.length;
    },
    (offset: number, data: Uint8Array) => {
      access.write(data, { at: offset });
    },
  );

  const p = info.picture;
  const fps = p.rateNum / p.rateDen;
  const [minQuality, maxFrame, optimal] = job.limits();
  const limits = { minQuality, maxFrame, optimal: optimal === 1 };
  const workers = Math.max(1, Math.min(8, (navigator.hardwareConcurrency || 4) - 1));
  const pool = new EncoderPool(workers);
  const inflightLimit = Math.ceil(fps) + 4 * workers;
  const canvas = new OffscreenCanvas(p.storedWidth, p.storedHeight);
  const ctx = canvas.getContext("2d")!;
  ctx.imageSmoothingQuality = "high";
  const expected = info.duration ? info.duration * fps : null;

  let failure: unknown = null;
  const fail = (err: unknown) => {
    failure ??= err;
  };
  let emitted = 0;
  let written = 0;
  let encoding = 0;
  const tasks = new Set<Promise<void>>();
  const track = (task: Promise<void>) => {
    tasks.add(task);
    task.catch(fail).finally(() => tasks.delete(task));
  };

  const dispatch = (finished: boolean) => {
    for (let d = job.nextDecision(finished); d; d = job.nextDecision(finished)) {
      const { index, quality } = d;
      d.free();
      encoding += 1;
      track(
        pool.encode(index, quality, limits).then((frame) => {
          encoding -= 1;
          written = job.addFrame(index, frame.data, frame.quality, frame.fits);
          if (expected) progress(Math.min(1, written / expected));
        }),
      );
    }
  };

  const emit = (rgba: ArrayBuffer) => {
    const index = emitted++;
    track(
      pool.analyze(index, rgba, p.storedWidth, p.storedHeight).then((words) => {
        job.pushModel(index, words);
        dispatch(false);
      }),
    );
  };

  let held: ArrayBuffer | null = null;
  let lastPts = 0;
  let lastDuration = 0;
  let frames = Promise.resolve();
  const render = (frame: VideoFrame): ArrayBuffer => {
    ctx.save();
    const [dw, dh] = transform(ctx, info, videoTrack.rotation);
    ctx.drawImage(frame, 0, 0, dw, dh);
    ctx.restore();
    return ctx.getImageData(0, 0, p.storedWidth, p.storedHeight).data.buffer;
  };
  const onFrame = (frame: VideoFrame) => {
    const repeats = job.repeats(frame.timestamp);
    for (let i = 0; i < repeats && held; i++) emit(held.slice(0));
    held = render(frame);
    lastPts = frame.timestamp;
    lastDuration = frame.duration ?? 0;
    frame.close();
  };
  const decoder = new VideoDecoder({
    output: (frame) => {
      frames = frames.then(() => onFrame(frame)).catch(fail);
    },
    error: fail,
  });
  decoder.configure(decoderConfig(media, videoTrack)!);

  let audioDecoder: AudioDecoder | null = null;
  let audioEncoder: AudioEncoder | null = null;
  let converter: AudioConverter | null = null;
  let audioTime: number | null = null;
  let flushAudio = () => {};
  let primingLeft: number | null = null;
  const sampleRate = info.audio.sampleRate ?? 0;
  const channels = info.audio.channels ?? 0;
  if (info.audio.action === "encode" && audioIndex !== null) {
    const source = tracks[audioIndex];
    converter = new AudioConverter(source.sampleRate, source.channels, sampleRate, channels);
    const encodeSamples = (planar: Float32Array) => {
      const frames = planar.length / channels;
      if (frames === 0 || !audioEncoder) return;
      const data = new AudioData({
        format: "f32-planar",
        sampleRate,
        numberOfFrames: frames,
        numberOfChannels: channels,
        timestamp: audioTime!,
        data: planar as Float32Array<ArrayBuffer>,
      });
      audioTime! += (frames * 1e6) / sampleRate;
      audioEncoder.encode(data);
      data.close();
    };
    audioEncoder = new AudioEncoder({
      output: (chunk, meta) => {
        const description = meta?.decoderConfig?.description;
        if (description) {
          const bytes = ArrayBuffer.isView(description)
            ? new Uint8Array(description.buffer, description.byteOffset, description.byteLength)
            : new Uint8Array(description);
          job.setAudioConfig(bytes.slice());
        }
        const data = new Uint8Array(chunk.byteLength);
        chunk.copyTo(data);
        job.addAudio(data, chunk.timestamp, AAC_FRAME);
      },
      error: fail,
    });
    audioEncoder.configure(aacConfig(info));
    audioDecoder = new AudioDecoder({
      output: (data) => {
        const skip = Math.min(data.numberOfFrames, primingLeft ?? 0);
        primingLeft = (primingLeft ?? 0) - skip;
        const frames = data.numberOfFrames - skip;
        if (frames === 0) {
          data.close();
          return;
        }
        audioTime ??= 0;
        const planar = new Float32Array(frames * data.numberOfChannels);
        for (let c = 0; c < data.numberOfChannels; c++) {
          data.copyTo(planar.subarray(c * frames, (c + 1) * frames), {
            planeIndex: c,
            frameOffset: skip,
            frameCount: frames,
            format: "f32-planar",
          });
        }
        data.close();
        encodeSamples(converter!.process(planar));
      },
      error: fail,
    });
    audioDecoder.configure(audioDecoderConfig(media, source)!);
    flushAudio = () => encodeSamples(converter!.flush());
  }
  const audioSource = audioIndex === null ? null : tracks[audioIndex];

  const checkAll = () => {
    check();
    if (failure) throw failure;
  };
  try {
    for (let packet = media.nextPacket(); packet; packet = media.nextPacket()) {
      checkAll();
      const data = packet.data;
      if (packet.track === info.videoIndex) {
        decoder.decode(
          new EncodedVideoChunk({
            type: packet.key ? "key" : "delta",
            timestamp: packet.pts,
            duration: packet.duration,
            data,
          }),
        );
      } else if (audioDecoder) {
        primingLeft ??= Math.max(0, Math.round((-packet.pts * tracks[audioIndex!].sampleRate) / 1e6));
        audioDecoder.decode(
          new EncodedAudioChunk({ type: "key", timestamp: packet.pts, duration: packet.duration, data }),
        );
      } else if (audioSource) {
        const samples =
          audioSource.codec === "aac"
            ? AAC_FRAME
            : Math.round((packet.duration * audioSource.sampleRate) / 1e6) || 1152;
        job.addAudio(data, packet.pts, samples);
      }
      packet.free();
      await until(
        () =>
          decoder.decodeQueueSize < DECODE_QUEUE &&
          emitted - written < inflightLimit &&
          (audioDecoder?.decodeQueueSize ?? 0) + (audioEncoder?.encodeQueueSize ?? 0) < AUDIO_QUEUE,
        checkAll,
      );
    }
    await decoder.flush();
    await frames;
    checkAll();
    const end = lastPts + (lastDuration || 1e6 / fps);
    const repeats = job.finishFrames(end);
    for (let i = 0; i < repeats && held; i++) emit((held as ArrayBuffer).slice(0));
    if (audioDecoder && audioEncoder) {
      await audioDecoder.flush();
      flushAudio();
      await audioEncoder.flush();
    }
    while (written < emitted) {
      await until(() => tasks.size === 0 || written >= emitted, checkAll);
      dispatch(true);
      if (encoding === 0 && tasks.size === 0 && written < emitted) {
        throw new Error("frames were left unencoded");
      }
    }
    checkAll();
    const report: string[] = JSON.parse(job.finish());
    access.flush();
    access.close();
    progress(1);
    return { kind: "done", report };
  } catch (err) {
    access.close();
    await root.removeEntry(temp).catch(() => {});
    if (err instanceof Cancelled) return { kind: "cancelled" };
    return { kind: "failed", message: err instanceof Error ? err.message : String(err) };
  } finally {
    pool.close();
    for (const codec of [decoder, audioDecoder, audioEncoder]) {
      if (codec && codec.state !== "closed") codec.close();
    }
    converter?.free();
    media.free();
    cancelled.delete(id);
  }
}

async function handle<M extends keyof Requests>(
  method: M,
  args: Requests[M]["args"],
  id: number,
  progress: (fraction: number) => void,
): Promise<Requests[M]["result"]> {
  await ready;
  switch (method) {
    case "presets":
      return JSON.parse(presets());
    case "help":
      return JSON.parse(help());
    case "plan": {
      const a = args as Requests["plan"]["args"];
      return plan(a.files, a.keys, a.settings);
    }
    case "convert": {
      const a = args as Requests["convert"]["args"];
      return convert(id, a.file, a.settings, a.temp, progress);
    }
  }
  throw new Error(`unknown method ${method}`);
}

self.onmessage = async (event: MessageEvent<Message>) => {
  const message = event.data;
  const reply = (r: Reply) => self.postMessage(r);
  if (message.method === "cancel") {
    cancelled.add(message.id);
    return;
  }
  try {
    const result = await handle(message.method, message.args, message.id, (progress) =>
      reply({ id: message.id, progress }),
    );
    reply({ id: message.id, result });
  } catch (err) {
    reply({ id: message.id, error: err instanceof Error ? err.message : String(err) });
  }
};
