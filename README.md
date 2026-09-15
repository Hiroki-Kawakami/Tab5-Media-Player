# Tab5-Media-Player

Play video files on M5Stack Tab5 (ESP32-P4).

Clone with the required submodule:

```sh
git clone --recurse-submodules https://github.com/Hiroki-Kawakami/Tab5-Media-Player.git
cd Tab5-Media-Player
```

For an existing checkout, run `git submodule update --init --recursive` once.

## Development

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
