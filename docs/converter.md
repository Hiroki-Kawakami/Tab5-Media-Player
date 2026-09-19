# Converter (`tools/converter-rs`)

`tab5conv` turns an arbitrary video into a file the player is guaranteed to
open. For now the only video codec is H.264 (4:2:0 8-bit) and the only audio
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
codec's own module interprets the keys (`video.rs`, `audio.rs`; the shared
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
codec supplies its own constraints: the rounding unit (2 for H.264) and the
player's limits for that codec. H.264's limits are each side at most 1280 and
at most 3600 macroblocks.

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
