# Converter (`tools/converter-rs`)

`tab5conv` turns an arbitrary video into a file the player is guaranteed to
open. For now it writes only H.264 (High, 4:2:0) + AAC, to MP4 by default or to
MKV when the output ends in `.mkv`.

```sh
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- in.mov
nix develop -c cargo run --manifest-path tools/converter-rs/Cargo.toml -- in.mov -o out.mp4 --long-side 1280 --dry-run
```

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

The limits are the player's, not ffmpeg's:

- **The long side defaults to 640**, because the decoders are tuned for 360p
  (see [`h264.md`](h264.md)). It is capped at 1280.
- **Large squares are shrunk further.** The macroblock limit (3600, same as the
  decoder) can bind before the side limit, so a 1280x1280 source comes out
  smaller than 1280.
- **Sources are never upscaled.**
- **The output has square pixels.** The player ignores sample aspect ratio, so
  anamorphic sources are scaled to their display aspect and written with
  `setsar=1`.
- **Rotation is baked into the pixels.** ffprobe's display-matrix rotation swaps
  the display size, and ffmpeg's default autorotate applies it and drops the
  matrix, so the output needs no rotation handling in the player.

The first video stream that is not cover art (`attached_pic`) is used. Audio
over 2 channels is downmixed, and audio over 48 kHz is resampled to 48 kHz.
