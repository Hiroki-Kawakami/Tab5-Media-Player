# Converter (`tools/converter-rs`)

`tab5conv` turns an arbitrary video into a file the player is guaranteed to
open. Video is H.264 or progressive MPEG-2 (4:2:0 8-bit), and the only audio
encoder is AAC-LC. Output is MP4 by default, or MKV when the output name ends in
`.mkv`.

```sh
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- in.mov
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- in.mov -o out.mp4 \
    --video "h264,long=1280,short=720,scale=fit,crf=20" --audio "aac,bitrate=96k" --dry-run
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- --video help
```

## Codec specs

`--video` and `--audio` each take one string, `<codec>[,key=value]...`, and the
codec's own module interprets the keys (`video/`, `audio.rs`; the shared
parsing is in `spec.rs`). One flag per codec option would multiply as codecs are
added, and most options only mean something for one codec. Every key is
checked: an unknown key, a repeated key or a bad value is an error, and the
message lists the keys the codec accepts. Nothing is silently ignored, with
the one intended exception described under `auto` below.

`--audio auto`, the default, copies the input track when the player can decode
it as is. That means AAC-LC or MP3 with at most 2 channels and 48 kHz. Anything
else is encoded as AAC-LC.
HE-AAC is re-encoded because only LC is copied. `auto` takes the `aac` keys,
but they only apply when it ends up encoding.

## MPEG-2

ffmpeg's `mpeg2video` defaults are wrong for the player: 200 kbit/s, no B
pictures and a 12-frame GOP. So `mpeg2` sets every one of those itself
(`qscale=4`, 2 B pictures, 2 s GOP).

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

## ffmpeg is a command, not a library

The tool runs the user's `ffmpeg`/`ffprobe` from `PATH` instead of linking
libav*. If we linked it, we would distribute ffmpeg ourselves: libx264 would make
any binary we ship GPL, and a static LGPL build would bring relinking
obligations. Calling the command puts none of that on this repo and removes the
static ffmpeg build from the flake. What we pay for it is a dependency on
whatever ffmpeg the user has. `require_encoders` checks
`ffmpeg -encoders` up front, so a build without `libx264` fails with a clear
message instead of partway through.

The MJPEG encoder that is planned will be written in-house so it can pick a
quality per frame. It will sit between two ffmpeg processes connected by pipes:
raw YUV comes out of the first, and the second muxes the result with
`-c:v copy`.

## Output size

The size keys (`size.rs`) are shared by every video codec. `long`/`short` exist
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

The first video stream that is not cover art (`attached_pic`) is used.
