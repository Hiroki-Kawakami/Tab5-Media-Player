# Converter (`tools/converter-rs`)

`tab5conv` turns an arbitrary video into a file the player is guaranteed to
open. Video is MJPEG (the default), H.264 or progressive MPEG-2 (4:2:0 8-bit),
and audio is AAC-LC or MP3. Output is MP4 by default, or MKV when the output name ends in
`.mkv`.

```sh
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- in.mov
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- in.mov -o out.mp4 \
    --video "h264,long=1280,short=720,scale=fit,crf=20" --audio "aac,bitrate=96k" --dry-run
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- --video help
```

## Crates

The CLI is one of several front ends planned (a native GUI and a browser
version), so the tool is a workspace:

| crate | holds |
|---|---|
| `crates/core` | spec parsing, presets, sizes, frame rates, audio copy/encode decisions, the MJPEG encoder and its rate control, the browser's MPEG-2 encoder (`mpeg2/`), frame scaling and colour conversion (`resample.rs`, `yuv.rs`), MP4/MKV demuxing and MP4 muxing (`container/`) |
| `crates/ffmpeg` | ffprobe, turning a plan into ffmpeg arguments, the MJPEG pipeline, batch output naming |
| `crates/cli` | `tab5conv` itself |
| `crates/gui` | the desktop app (Tauri); its UI is `web/` |
| `crates/wasm` | `core` for the browser version (wasm-bindgen) |

- **A plan is values, not ffmpeg arguments.** `VideoPlan`/`AudioPlan` say
  what to produce (stored size, crop, rotation, frame-rate conversion,
  colour, codec parameters, copy or encode); only `crates/ffmpeg/args.rs`
  turns them into filters and flags. A browser version maps the same plan to
  WebCodecs, so the player's rules live in one place and cannot drift
  between front ends.
- **`core` uses no processes, threads or file I/O**, so it builds for
  `wasm32-unknown-unknown`. The MJPEG encoder's stages (`jpeg::analyze`,
  `RateControl`, `jpeg::encode`) are plain functions; the threads that run
  them belong to `crates/ffmpeg/pipeline.rs`.
- **x264 settings are in `crates/ffmpeg`.** `-x264-params` is an x264
  detail, while `core` only says which profile.

## Desktop app

```sh
nix develop -c bash -c 'cd tools/converter-rs/web && npm ci'        # once
nix develop -c bash -c 'cd tools/converter-rs/crates/gui && cargo tauri dev'
nix develop -c bash -c 'cd tools/converter-rs/crates/gui && cargo tauri dev --release'
nix develop -c bash -c 'cd tools/converter-rs/crates/gui && cargo tauri build --bundles app'
```

- **Use `--release` to try real conversions.** The built-in MJPEG encoder
  is unoptimised in a dev build and far slower than the CLI's release build.
- **The UI in `web/` is shared with the planned browser version.** It only
  talks to a `Backend` (`web/src/backend.ts`): `native.ts` calls the Tauri
  commands in `crates/gui`, and `mock.ts` fakes everything, so
  `npm run dev` and `?backend=mock` show the UI in a plain browser.
- **ffmpeg is still the user's, not bundled** (see
  [ffmpeg is a command](#ffmpeg-is-a-command-not-a-library)). An app
  started from Finder does not get the shell's `PATH`, so `location.rs`
  also searches the Homebrew, MacPorts and nix profile folders. A folder
  chosen in the app is kept in `settings.json` in the app's config
  directory.
- **Monitored runs differ from the CLI's only in logging.** They pass
  `-nostats -progress pipe:1` and keep the last lines of ffmpeg's stderr for
  the error message; the MJPEG pipeline reports progress from the frames it
  has written. The CLI keeps `-stats` and the inherited stderr, and
  `--dry-run` prints the CLI's commands.
- **A cancelled or failed run leaves nothing behind**, through the same
  `.part` file as the CLI. Inputs are converted one at a time, as in the CLI.
- **IPC tests (`crates/gui/src/main.rs`) use `tauri::test`.** The request
  URL must be the platform's local origin (`tauri://localhost` outside
  Windows and Android); with `http://tauri.localhost` on macOS every command
  is refused as "not allowed. Plugin not found". `generate_context!` may
  appear only once per crate (it defines `_EMBED_INFO_PLIST`), hence
  `context()`.
- **An app built in `nix develop` is not distributable**: it links
  `libiconv` from `/nix/store`. Release builds need a non-nix toolchain.
- **Under the nix toolchain a panicking test aborts the whole test binary**
  ("failed to initiate panic, error 5"). The message of the first failure is
  still printed; the tests after it do not run.

## Browser version

```sh
nix develop -c bash -c 'cd tools/converter-rs/web && npm run dev'         # http://localhost:5173
nix develop -c bash -c 'cd tools/converter-rs/web && npm run e2e'         # converts in Chrome, checks against the CLI
nix develop -c bash -c 'cd tools/converter-rs/web && npm run check:wasm'  # wasm (SIMD) and native output are identical
nix develop -c bash -c 'cd tools/converter-rs/web && npm run bench [clip...]'  # time per stage, see below
nix develop -c bash -c 'cd tools/converter-rs/web && npm run bench:kernels'    # wasm kernels in node, with output hashes
```

The same UI runs with `web/src/browser/` as its backend: an engine worker
(demux, WebCodecs decoding, rate control, muxing), scale workers (each
decoded frame's YUV to the stored picture) and a pool of encoder workers
(MJPEG analysis and encoding, or whole MPEG-2 GOPs). The output is written
to the origin private file system and then downloaded.

- **Containers are our own, not ffmpeg.wasm or a JS library** (licence and
  size). `core/src/container/` reads MP4/MOV and MKV/WebM and writes MP4.
  `core/tests/container.rs` checks every packet (size, key flag, pts,
  adler32) against `ffprobe -show_packets -show_data_hash`, and
  `ffmpeg/tests/media_info.rs` checks that the demuxed `MediaInfo` gives the
  same plans as ffprobe for every preset.
- **The MP4 is written with `moov` at the end**, so it can be streamed to
  disk; only the `mdat` size is patched afterwards. The player finds `moov`
  with one seek (see [`playback.md`](playback.md)).
- **Samples are written in time order across tracks** (`container/interleave.rs`),
  as ffmpeg's muxer does. The player reads ahead only 16 x 64 KB, so audio
  written seconds after the video of the same time stalls playback a moment
  in; an early version wrote each video frame as soon as it was encoded and
  put audio 2.4 s behind at a 2.7 MB/s bitrate. WebCodecs audio runs
  behind the video, so the engine also stops demuxing while the audio
  decoder and encoder queues are long; the interleaver's own limit
  (256 MiB queued) is only a guard against a track that stops producing.
  `npm run e2e` checks the lag against the CLI's. The video timescale is
  the CLI's: 1/1200000 for MJPEG, and for MPEG-2 the one ffmpeg picks (the
  rate's numerator, doubled until it reaches 10000).
- **Only MJPEG and MPEG-2 are written; H.264 is left to the CLI and the
  desktop app** (`tiny`). The browser's own H.264 encoder may be
  Baseline-only, and without B pictures the player has nothing to drop when
  it runs late (see [H.264](#h264)); `small` already converts in the browser
  at several times real time. MPEG-2 has its own
  encoder, see [below](#the-browsers-mpeg-2-encoder). MJPEG uses the CLI's
  encoder and rate control (`core/src/mjpeg.rs` holds the loop both use). Only size models travel
  between workers; each frame's coefficients stay in the worker that will
  encode it.
- **Frames are taken as YUV, not drawn to a canvas.** `scale.worker.ts`
  gets each `VideoFrame`, copies its planes out with `copyTo` (I420, NV12,
  I422 and I444), and `core/src/yuv.rs` scales, rotates, crops and
  converts them into the stored picture: the source's own values for
  MPEG-2, BT.601 full range for MJPEG. Only frames `copyTo` cannot give in
  8-bit YUV go through the canvas and RGB. `core/tests/resample.rs` checks
  the result against ffmpeg's `scale`/`crop`/`transpose` chain (67-75 dB)
  and its matrix conversion (48-58 dB).
  - **Chrome's canvas cannot downscale.** `drawImage` and
    `createImageBitmap` always resample with a 2x2 bilinear, whatever
    `imageSmoothingQuality` or `resizeQuality` say (`"pixelated"` gave
    byte-identical output). Beyond 2x pixels are skipped: a pattern with
    every third pixel white came out all black at 1/3, and 1080p to 360p
    broke thin lines. `core/src/resample.rs` is a separable bicubic like
    ffmpeg's default `scale` (49-54 dB against it); the canvas fallback uses
    it too for reductions beyond 2x.
  - **Chrome's colour conversion cannot be steered.** It decodes untagged
    HD H.264 as BT.709, while ffmpeg (and so the CLI) takes it as BT.601,
    and it ignored a `colorSpace` in `VideoDecoderConfig` for H.264 on
    macOS (passing BT.601, a matrix only or nothing gave byte-identical
    output). Through RGB an untagged 1080p clip came out at 23.6 dB against
    the source instead of the CLI's 31.0, and MPEG-2 lost 1-1.5 dB of
    chroma to the round trip. The colour now comes from the container's
    tags or the H.264 SPS (`container/h264.rs`; ffmpeg's MP4s carry it only
    there), untagged H.264 is BT.601 limited range as in ffmpeg, and other
    untagged codecs fall back to the frame's `colorSpace`. Container tags
    are still passed to the decoder for the canvas fallback: Chrome ignored a
    WebM's `smpte170m` otherwise.
  - **No `willReadFrequently` on the fallback canvas.** It makes the canvas
    CPU-backed and `drawImage` then took 16 ms a frame.
  - **The scale workers get at most 8 frames at a time**; decoder frames
    held elsewhere can stall the decoder. More scale workers (6 or 8) did
    not convert faster: the decoder and `copyTo` (about 5 ms a 1080p frame)
    were then the limit.
- **Chrome encodes AAC only at 44.1 and 48 kHz** (checked on macOS). The
  browser plans audio with `AudioSpec::plan_for`, which raises an unset
  sample rate to the next supported one and rejects an explicit one it
  cannot encode.
- **Priming.** Chrome's `AudioDecoder` does not carry the negative
  timestamps of an AAC input's priming, so the decoded samples are trimmed
  by the first packet's demuxed pts. Copied audio keeps its priming behind
  an edit list, as ffmpeg writes it. The encoder's own priming (2112
  samples, 44 ms, from AudioToolbox) is not compensated: there is no
  per-platform value to rely on.
- **Frame rate conversion** (`FrameSelector`) keeps, for each output slot,
  the last frame whose pts rounds to it, like ffmpeg's `fps` filter.
- **`npm run bench` says which stage limits the speed.** The engine logs a
  `[timing]` line per conversion: the workers' busy time per stage and how
  long the demux loop waited, by the condition that held it (`wait:scaler`,
  `wait:encoder`, ...). Busy time near 100% of the wall time times the
  worker count marks the bottleneck. It converts a 30 s 1080p `testsrc2`
  clip unless clips are given; `testsrc2` is easier to encode than real
  footage, so compare runs on the same clip; runs vary by about 10%. Like
  `e2e`, it serves `dist/`.
- **Hot loops have a wasm SIMD version next to the scalar one**
  (`mod wasm` under `target_feature = "simd128"`): the resampler, colour
  conversion, forward DCT, JPEG quantisation and size model, MPEG-2 SAD.
  Native builds, the CLI included, run the scalar code, and
  `npm run check:wasm` checks that both give the same bytes. So a SIMD
  path must do the same float operations in the same order per lane, with
  no FMA; relaxed SIMD is not used because its results vary by machine.
  `bench:kernels` prints a hash of each kernel's output to show whether a
  change altered it.
- **`f32::round` is a libm call in wasm**, since no instruction rounds half
  away from zero. `num::round` builds it from `trunc` with the same results
  (checked against `f32::round`); `roundf` alone had been 5% of MJPEG time.
- **No WebGPU.** Scaling is the only stage that suits it, and on machines
  where the GPU is not on shared memory the upload and readback would cost
  what the shader saves.
- **Build.** The `wasm-bindgen` crate is pinned to the nix CLI's version;
  they must match exactly. lld is not put on `PATH` (it could change other
  builds in the shell); `CARGO_TARGET_WASM32_UNKNOWN_UNKNOWN_LINKER` points
  at its `wasm-ld` instead.
- **`npm run e2e` uses the installed Google Chrome** through
  `playwright-core`; Playwright's own Chromium has no H.264 or AAC. It
  serves `dist/`, so run `npm run build` first. It converts the inputs with
  the `default` and `small` presets and compares both with the CLI, all
  three planes above 35 dB.

## Several inputs

`tab5conv *.mp4 --outdir out` converts one file after another, not in
parallel: the MJPEG encoder and x264 each use every core already.

- **Outputs are `<input>.tab5.mp4` next to the input, or `DIR/<input>.mp4`
  with `--outdir`.** `-o` takes a single input. With several inputs, a name
  containing `.tab5.` is skipped as an earlier output, so re-running a glob
  over the same folder does not convert outputs again.
- **Everything that can fail before encoding is checked up front**: two
  inputs writing the same file, an output that is its own input (`--outdir`
  pointing at the input folder), and, without `-y`, outputs that already
  exist. Nothing is converted until all of them pass, so it is clear which
  files `-y` would replace.
- **A failing input does not stop the batch.** Each output is written to
  `<output>.part.<ext>` and renamed when ffmpeg succeeds, and a failed part
  is deleted. A half-written file is never taken for a finished one. The run
  ends with a list of skipped and failed inputs and exits non-zero if any
  failed.

## Codec specs

`--video` and `--audio` each take one string, `<codec>[,key=value]...`, and the
codec's own module interprets the keys (`core/src/video/`, `core/src/audio.rs`;
the shared parsing is in `core/src/spec.rs`). One flag per codec option would multiply as codecs are
added, and most options only mean something for one codec. Every key is
checked: an unknown key, a repeated key or a bad value is an error, and the
message lists the keys the codec accepts. Nothing is silently ignored, with
the one intended exception described under `keep` below.

`--audio aac` (the default) and `--audio mp3` take `keep=auto|none`. With
`auto`, an input that is already the chosen codec is copied, provided the
player can take it as is: AAC-LC (not HE-AAC) or MP3, at most 2 channels,
16-48 kHz. It must also be no bigger than the bitrate an encode would use,
whether set or the codec's default, so `bitrate` caps the file even when
the codec matches. ffprobe's figure is a little off (a 128k CBR track reads
127-128.1k), hence 5% slack. The bitrate comes from the stream or, in MKV,
from a `BPS` tag; MKVs written by ffmpeg have neither, and an unknown
bitrate is encoded. `vbr` has no bitrate to compare, so it skips the check.
Anything else is encoded, including the other codec, so the output codec is
always the one asked for. There is no `keep=always`, which would
copy tracks the player may not handle. The encoder keys only apply when the
track ends up encoded.

**Output audio is 16-48 kHz, whatever the codec.** On the device, driving the I2S
output at 8 kHz makes audible noise. Lower-rate input is therefore resampled
up to 16 kHz, and `keep=auto` does not copy it. The device's MP3 decoder itself
played every rate down to 8 kHz, MPEG-2.5 included.

## Presets

`--preset` (`core/src/preset.rs`) supplies the `--video` and `--audio` strings:
`tiny`, `small`, `default` (the default) and `quality`; `--preset help` lists
them. Presets are plain spec strings run through the same parser, so they
cannot drift from the options. An explicit spec with the same codec is merged
over the preset's (its keys win), so `--preset tiny --video h264,crf=20`
only changes the CRF. One with another codec replaces that side, because the
preset's keys would not make sense for it. `default` names only the codecs,
so it follows the codecs' own defaults when they change. The run prints the
merged specs, so it is clear what was applied.

## MP3

- **A CBR bitrate must be one MP3 can signal at the output sample rate**
  (32k-320k from 32 kHz up, 8k-160k below). lame would otherwise round it to
  the nearest one without saying so. The check runs after probing, because the
  sample rate defaults to the input's.
- **The default CBR is 192k, or 160k below 32 kHz**, because 192k does not
  exist there and a default must never be an error.
- **The 16 kHz floor also keeps MPEG-2.5 MP3 (8-12 kHz) out**, which ffmpeg's
  mp4 muxer rejects as non-standard.

## H.264

The defaults are `profile=main,crf=32,keyint=4`. x264 also always gets
`bframes=3:b-pyramid=none:ref=1:weightp=0`; with `profile=baseline` it gets
only `ref=1`, because `bframes` in `-x264-params` would override the
profile and turn the stream back into Main. The target is a stream that
decodes no heavier per frame than a 360p YouTube encode (Main, flat B
pictures), which plays well on the device. The figures below are for a
164 s 360p clip from YouTube (605 kbit/s, 2.5 KB a frame, 59% non-reference
slices, 1150 fps in the host decoder).

- **Frames the player can drop matter more than raw decode speed.** When it
  runs late, the player skips pictures with `nal_ref_idc == 0` undecoded
  and otherwise has to wait for the next keyframe (see
  [`playback.md`](playback.md#h264-playback)). Baseline has no B pictures,
  so 0% of it can be dropped. It decoded fast on the host (1136 fps) and
  still fell behind on the device. B-pyramids make some B pictures
  references (x264's default Main left 47% droppable); flat B pictures with
  one reference give 61%.
- **Lowering the frame rate does not lower the load.** At the same CRF,
  15 fps saved only 7% of the bitrate and doubled the size of each frame
  (5.2 KB against 2.8 KB), because consecutive frames differ more. Per-frame
  load is what the player has to keep up with, so `tiny` stays at 30 fps and
  saves size through CRF.
- **CRF 32 is a little lighter than the reference per frame**: 510 kbit/s,
  2.1 KB a frame (P 3.7 KB, B 0.9 KB), 61% droppable, 1210 fps on the host,
  33.7 dB against the YouTube file. CRF 30 matches it (604 kbit/s, 2.5 KB,
  35.2 dB), and CRF 28 is 7% heavier than that at 36.7 dB. The old defaults (High,
  CRF 23, 2 s keyframes, 15 fps in `tiny`) came out at 1283 kbit/s and 539 fps.
- **Keyframes every 4 s**: I pictures were over a tenth of the file at 2 s.
  Seeking snaps to keyframes, so it gets coarser.

## MPEG-2

ffmpeg's `mpeg2video` defaults are wrong for the player: 200 kbit/s, no B
pictures and a 12-frame GOP. So `mpeg2` sets every one of those itself
(`qscale=8`, 2 B pictures, 2 s GOP).

- **`qscale=8` matches MJPEG's default quality.** On 30 s of a detailed 4K
  clip scaled to 360p, `qscale=8` gave 3.0 Mbit/s at 34.3 dB, the same as
  `mjpeg,quality=75` at 10 Mbit/s (34.5 dB). The old default of 4 gave
  6.1 Mbit/s at 39.3 dB, so it barely came out smaller than MJPEG. To
  compare MJPEG with the others, convert its BT.601 full-range output to
  the source's matrix first (`scale=in_color_matrix=bt601:in_range=full:...`).
  Comparing raw YUV of mismatched matrices cost it 8 dB.

- **`bframes` stops at 3**, the most the decoder has been checked against.
  B pictures are also what the player drops when it runs late.
- **GOPs are closed by default.** After a seek the decoder drops B pictures
  that refer to the previous GOP, so an open GOP loses a few frames after
  every seek. ffmpeg refuses closed GOPs while scene-change detection is on, so
  `gop=closed` also passes `-sc_threshold 1000000000`, and cuts get no I
  picture of their own. `gop=open` brings scene-change detection back.
- **`hq=yes` adds `-mbd rd -trellis 1 -intra_vlc 1`**, which only changes the
  encoder's choices and costs the decoder nothing. On 640x360 real footage it
  cut the size by 11% at `qscale=4` (PSNR 0.3 dB lower), gave +0.2 dB at the
  same bitrate, and roughly doubled encode time.
- **Interlace tools are never enabled.** The decoder rejects field pictures and
  field prediction, and `-alternate_scan` makes ffmpeg flag frames as
  non-progressive (see [`mpeg2.md`](mpeg2.md#scope)).
- **The frame rate is left as it is.** `mpeg2video` accepts rates that MPEG-2
  has no code for (15 and 12 fps, for example), and the player times frames by
  the container's timestamps anyway.
- **GOPs stop at 600 frames.** `mpeg2video` cuts anything longer to 600 with
  a warning, so the plan does the same and the label says what is written.

To check an output against the player's decoder, copy the stream out with
`-c copy -f mpeg2video` and follow the bit-exact procedure in
[`mpeg2.md`](mpeg2.md#verifying).

### The browser's MPEG-2 encoder

`core/src/mpeg2/` writes what the player's decoder takes and nothing more:
frame pictures, frame prediction and frame DCT, one slice per macroblock
row, the default matrices, `intra_vlc_format=1`, half-pel motion. The CLI
keeps using `mpeg2video`.

```sh
nix develop -c cargo build --release --manifest-path tools/converter-rs/Cargo.toml -p tab5conv-core --example mpeg2
tools/converter-rs/target/release/examples/mpeg2 640 360 30 8 2 60 1 out.m2v recon.yuv < in.yuv   # W H FPS QSCALE BFRAMES KEYINT HQ
```

- **It reconstructs with the same integer IDCT as `ffmpeg -idct simple`**,
  mismatch control included, so its reference pictures are byte-identical
  to what `mpeg2_dec` and ffmpeg decode and nothing drifts.
  `core/tests/mpeg2.rs` checks the reconstruction against ffmpeg for the
  elementary stream and the muxed MP4. The example writes the
  reconstruction, to compare with `mpeg2_dec_test` as in
  [`mpeg2.md`](mpeg2.md#verifying); I/P/B, 1-3 B pictures, `qscale` 1-31,
  640x360, 1280x720, 720x1280 and 350x198 all matched.
- **`hq` means rate-distortion decisions**: the two candidates (intra,
  forward, backward, both) with the lowest SAD cost, and skip, are coded
  and the cheapest `SSE + λ·bits` wins, with a trellis over each block's
  run/level codes. λ is `0.85·qscale²`, what `mpeg2video` uses with
  `-mbd rd`. Coding every candidate took 1.5x the time for output
  0.06-0.4% smaller and at most 0.03 dB better; one candidate was 2-3.5%
  larger. `hq=no` picks the mode by SAD.
- **An inter block with a small residual is not transformed.** No DCT
  coefficient exceeds a quarter of the block's absolute residual sum, so
  below four times the level-1 threshold every level is zero anyway
  (`small_inter_residuals_quantise_to_zero`); the output is unchanged.
- **Against `mpeg2video` with the CLI's settings** (`qscale=8`, 2 B
  pictures, `hq`), same input: 1.5% larger at the same PSNR on real footage
  scaled to 360p, 16-46% smaller on `testsrc2` at 360p, 720p and 720x1280.
- **`f_code` is chosen per picture** from the vectors motion estimation
  found (at most ±63.5 pixels), so slow pictures do not pay for range they
  do not use.
- **Vectors never point outside the coded picture.** A B picture's skip
  reuses the previous macroblock's vector, which can point outside at the
  current position; such a skip is not taken.
- **One worker encodes one closed GOP.** Splitting pictures by rows would
  need `SharedArrayBuffer`, i.e. COOP/COEP headers that GitHub Pages cannot
  send. So the browser rejects `gop=open` (a GOP would depend on the
  previous one). `bitrate=` is rejected too: `qscale` gives the sizes the
  player needs, so the browser has no rate control. Adding it would mean a
  budget per GOP decided before the GOP is dispatched, with the actual
  sizes fed back late.
- **Up to 256 MiB of frames are queued for the encoders.** Every worker
  needs its own GOP's frames waiting to be busy, so at 720p only about
  three GOPs of 60 frames are in flight. The encoders keep up with that:
  512 MiB converted no faster.
- **The source's matrix is kept and tagged** in `sequence_display_extension`;
  an untagged source stays untagged, as the CLI's `mpeg2video` output does.
- **Against the CLI** (`small`, ffmpeg-scaled source as the reference): the
  same PSNR within ±0.1 dB luma and slightly better chroma, at 59-89% of the
  bitrate, on `testsrc2` (tagged and untagged) and real footage.
- **Muxing with B pictures.** Samples are in decode order with `ctts`, and
  an edit list starts the track one frame in, as ffmpeg writes it. The
  edit's length is the end of the last composition time, not the sum of the
  sample durations; the sum cut the last frame off.
- **Speed** (Chrome, 10-core Mac, `npm run bench`): a 30 s 1080p clip
  converts to `small` in about 2.4 s and to 720p in about 4.2 s; the MJPEG
  default takes about 3 s. Before the SIMD and encoder work these were 3.9,
  11.3 and 7.4 s.

## MJPEG

The encoder is our own (`core/src/jpeg/`). That puts the choice of quantisation
between the DCT and the entropy coder, so the quality of each frame can be
decided from the whole timeline without encoding anything twice.
`ffmpeg/src/pipeline.rs` runs it between two ffmpeg processes:

```
ffmpeg (decode, fps, scale)
  -> reader
  -> analysis workers  (DCT, size model)            one per core
  -> rate controller   (quality per frame)          one thread, in order
  -> encode workers    (quantise, Huffman, bits)    one per core
  -> writer            (reorder, feed back sizes)   one thread
  -> ffmpeg -f mjpeg -framerate R -i pipe:0 (+ input audio, -c:v copy)
```

- **The output is always constant frame rate.** Raw frames carry no
  timestamps, so the muxer rebuilds them from `-framerate`. The `fps` filter is
  therefore always applied, at the input's own rate if nothing lowers it.
- **Frames are BT.601 full range**, which is what JFIF decoders assume. The
  scale filter converts with `out_color_matrix=bt601:out_range=full`.
- **Both worker stages run in parallel; only the controller is serial**, and
  it only evaluates size models. 270 frames of 720p take about 1.2 s. The DCT
  runs once per frame; its coefficients wait for the controller as `i16` at
  8x scale, about 2.8 MB per 720p frame.
- **The default size is the panel (`long=1280,short=720`).** The hardware
  decodes it in time, and 720x1280 portrait is the direct path. The limits are
  the renderer's: width at most 2560, at most 1920x1088 pixels.
- **Landscape output is stored turned 90° counter-clockwise, with
  display rotation -90** (`rotate`, `rotatewhen`, `rotatemeta`;
  `core/src/video/rotation.rs`). The direct path needs a 720x1280 source and an output
  rotation of 0, and output rotation is UI rotation plus source rotation. So
  a stored landscape clip decodes straight into the panel when the UI is at
  `rotate`'s angle (90 by default) and goes through PPA at the other
  landscape. The simulator log shows `direct decode` at `imu rot90` and
  `pipeline ... rotation 2` at `rot270`. Only MJPEG has a direct path, which
  is why the keys exist only here. Size keys describe the displayed picture;
  the limits are checked on the stored one. The metadata is
  `-display_rotation` on the piped input of the mux ffmpeg, which MP4 and MKV
  both keep.
- **The size model is a histogram of coefficients normalised by the Annex K
  base tables** (`core/src/jpeg/estimate.rs`). IJG scaling makes every quality's table
  "base x scale", so the size at any quality comes from the histogram
  alone: bits = 4.15 per non-zero AC + 1.06 x magnitude bits + 2.82 per block.
  The constants were fitted by least squares on two 720p clips at quality
  20-95. With them the estimate is 0.92-1.06 of the real size across that
  range; before fitting it ran from 0.55 to 0.87, and the slope is what
  matters, since the controller compares qualities with one model.
- **`bitrate` is a leaky bucket, and a soft one.** It drains `bitrate/8` bytes
  a second, and it may hold at most `buffer` bytes. The default buffer is the
  player's read-ahead, 16 x 64 KB. The controller looks one second ahead and
  gives each frame the highest quality at which that whole window, at one
  quality, stays within the bucket. That lowers quality ahead of a heavy
  stretch and keeps it from jumping about. Estimates are scaled by a running
  ratio of real to estimated size that the writer feeds back; on real
  footage the result is within about 1% on average. Nothing is guaranteed: a
  stretch that needs more than `minquality` gives goes over, and the report
  says so.
- **The controller's bucket is capped at `buffer`.** An overshoot that has
  already happened cannot be taken back. Without the cap, three seconds of
  noise left the bucket 10x full, and the easy frames after it stayed at
  `minquality` until it drained.
- **The default `bitrate` is 24M**: well above what ordinary footage needs at
  quality 80, above the default 75 (the 720p real-footage clip here needs
  about 11 Mbit/s), so it
  only stops outliers from growing the file. Audio is not counted: it is small next to
  the video, and counting it would tie video settings to audio ones.
- **`maxframe` defaults to the player's hard limit, 1 MiB** (the
  `media_buffer` bounce area; larger packets are skipped), and cannot be set
  above it. The controller respects it through the estimate. The one
  re-encode left is in the encode worker: a frame that really comes out over
  `maxframe` is re-quantised from its kept coefficients at the highest
  quality that fits. A frame over the limit even at `minquality` is written
  with a warning, and one over 1 MiB stops the run.
- **The forward DCT is libjpeg's float AAN butterfly** (`jfdctflt`), with
  the output scale applied afterwards. It replaced a direct matrix product;
  a 720p frame at quality 75 came out 8 bytes different. The MPEG-2
  encoder uses it too.
- **Optimal Huffman tables are per frame** (libjpeg's
  `jpeg_gen_optimal_table`), about 14% smaller than the Annex K tables on
  720p footage, with identical pixels.
- **The tree depth can exceed 32 before length limiting.** libjpeg sizes its
  histogram for 32; skewed counts can go deeper, so ours is sized to the real
  depth.
- **Quantisation uses the Annex K tables with IJG quality scaling**, so
  `quality` means what it means in libjpeg. They spend less on chroma than
  ffmpeg's `mjpeg` encoder, whose flatter tables score about 3 dB better in
  RGB PSNR at the same size (44.5 vs 45.0 dB luma, 39 vs 43 dB chroma at
  about 46 KiB per 720p frame). At `quality=100` the output is 69 dB PSNR
  against the encoder's input, so the encoder itself loses nothing.

To compare quality, decode both sides to raw frames first. The `psnr` filter
pairs frames by timestamp, and an MP4 (1/1200000 time base) against an MKV
source pairs the wrong frames and reports nonsense.

## ffmpeg is a command, not a library

The tool runs the user's `ffmpeg`/`ffprobe` from `PATH` instead of linking
libav*. If we linked it, we would distribute ffmpeg ourselves: libx264 would make
any binary we ship GPL, and a static LGPL build would bring relinking
obligations. Calling the command puts none of that on this repo and removes the
static ffmpeg build from the flake. What we pay for it is a dependency on
whatever ffmpeg the user has. `require_encoders` checks
`ffmpeg -encoders` up front, so a build without `libx264` fails with a clear
message instead of partway through.

MJPEG is the exception: its encoder is built in (see [MJPEG](#mjpeg)), so
ffmpeg only decodes and muxes.

## Output size

The size keys (`core/src/size.rs`) are shared by every video codec. `long`/`short` exist
so that one command line works for both landscape and portrait sources. Each
codec supplies its own constraints: the rounding unit and the player's limits
for that codec. H.264 and MPEG-2 share the same constraints: a rounding unit of
2, each side at most 1280, and at most 3600 macroblocks.

- **The default is `long=640,scale=contain`**, because the decoders are tuned
  for 360p (see [`h264.md`](h264.md)).
- **Only `contain` refuses to upscale.** `fit`, `cover` and `stretch` all allow
  it. Combining "no upscale" with `cover` or `stretch` had no clear meaning,
  so it is not offered.
- **Going over a limit is an error, not a silent shrink**, so an explicit size
  is never quietly changed. The error states the computed size.
- **The output has square pixels.** The player ignores sample aspect ratio, so
  sizes are computed on the display size and written with `setsar=1`.
- **Rotation is baked into the pixels.** ffprobe's display-matrix rotation swaps
  the display size, and ffmpeg's default autorotate applies it and drops the
  matrix, so the output needs no rotation handling in the player.

## Frame rate

The frame-rate keys (`core/src/framerate.rs`) are shared by every video codec, like the
size keys. `fps` converts to a constant rate, dropping or repeating frames.
`maxfps` only ever lowers the rate, the way `contain` only ever shrinks.

- **The default is `maxfps=30`**, for the same reason the default size is
  360p: the device decodes 360p High H.264 at about 33 fps, so a 60 fps
  source would drop frames all the way through.
- **`fps=` goes first in `-vf`**, so the scaler only sees frames that are
  kept.
- **`keyint` is converted with the output rate.** Using the input's would
  double the keyframe interval when 60 fps comes down to 30.
- **`23.976`, `29.97` and `59.94` mean the NTSC rates** (`24000/1001` and so
  on). As decimals they would be slightly off.
- **A source with no usable average rate is capped anyway.** `avg_frame_rate`
  can be `0/0`, and without the filter nothing would enforce the maximum.
  Sources at or below the maximum keep their timing, VFR included.

The first video stream that is not cover art (`attached_pic`) is used.
