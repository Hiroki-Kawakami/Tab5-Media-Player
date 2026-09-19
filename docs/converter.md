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
| `crates/core` | spec parsing, presets, sizes, frame rates, audio copy/encode decisions, the MJPEG encoder and its rate control |
| `crates/ffmpeg` | ffprobe, turning a plan into ffmpeg arguments, the MJPEG pipeline, batch output naming |
| `crates/cli` | `tab5conv` itself |

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

To check an output against the player's decoder, copy the stream out with
`-c copy -f mpeg2video` and follow the bit-exact procedure in
[`mpeg2.md`](mpeg2.md#verifying).

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
