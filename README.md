# Tab5-Media-Player

Open Video, Audio and Image files on M5Stack Tab5 (ESP32-P4).

## Supported Media Formats

### Video

Convert videos in your browser with the
[converter](https://hiroki-kawakami.github.io/Tab5-Media-Player/).
The converter works only in Chrome and Chromium-based browsers.  
The command-line version, `tab5conv`, is on the
[`converter-latest`](https://github.com/Hiroki-Kawakami/Tab5-Media-Player/releases/tag/converter-latest)
release; it needs `ffmpeg` and `ffprobe` on `PATH`.

- **Containers:** AVI, MKV, MP4 (`.mp4`, `.m4v`, `.mov`)
- **Video:** MJPEG (up to 2560 px wide), H.264 (Constrained Baseline/Main/High,
  progressive 4:2:0 8-bit, up to 1280x720), MPEG-2 (Simple/Main, progressive,
  up to 1280x720)
- **Audio:** PCM, MP3, IMA ADPCM, AAC, Opus (MKV and MP4 only)

### Audio

- `.mp3`, `.aac` (ADTS)
- `.wav` (16/24/32-bit PCM, IMA ADPCM)
- `.m4a` (AAC, MP3, Opus or PCM)

### Image

- JPEG (`.jpg`, `.jpeg`), baseline and progressive
- PNG (`.png`)

For the fastest display, use baseline JPEG no larger than the screen in the
orientation you view it (720x1280 portrait, 1280x720 landscape).

## Development

Clone with the required submodule:

```sh
git clone --recurse-submodules https://github.com/Hiroki-Kawakami/Tab5-Media-Player.git
cd Tab5-Media-Player
```

For an existing checkout, run `git submodule update --init --recursive` once.

The dev environment lives in a Nix flake; always run build tooling through
`nix develop`:

```sh
nix develop -c ./run.sh                                             # host simulator (SDL window)
nix develop -c ./run.sh esp32p4                                     # flash + monitor the device
nix develop -c ./run.sh esp32p4 build
nix develop -c ./run.sh simulator --verify simulator/verify/home.txt # headless UI check
```

`--verify` drives the simulator through the esp-devkit test harness and writes
JPEGs of the panel under `captures/`. The same scripts run against a flashed
board with `./run.sh esp32p4 --verify <port> <script>`.

Board support, the LVGL port and the UI framework come from the pinned
[`esp-devkit`](esp-devkit/) submodule.

See [`docs/architecture.md`](docs/architecture.md) for the component layout.

## License

MIT — see [`LICENSE`](LICENSE). This is a personal, non-commercial project
published as source on GitHub; it comes with no warranty and no support.
