# Tab5-Media-Player — Notes for Claude

Video file player for M5Stack Tab5 (ESP32-P4), built on the `esp-devkit`
submodule.

> **Keep the docs current.** When you change the build flow, hit a non-obvious
> gotcha worth remembering, add a target, or make a design decision that isn't
> derivable from the code, update [`docs/architecture.md`](docs/architecture.md)
> (or add a file under `docs/`) in the same change. Don't restate what the code
> already says; write only the *why*, the *why not*, and pointers to where to
> look. Don't record implementation plans or progress status here — `git log` is
> the record of what changed and when.

## Start here

- [`docs/architecture.md`](docs/architecture.md) — component layout and the
  device/simulator split.
- `esp-devkit/README.md` — the `devkit.cmake` macros (`devkit_idf_init`,
  `devkit_simulator`) shared across every esp-devkit-based project.
- `esp-devkit/docs/harness.md` — scripted UI verification: touch/button
  injection and panel capture, same script on simulator or board.
- `esp-devkit/ui_framework/inc/display_manager.hpp` — the LVGL display/touch
  manager; app code calls `display_manager.create_display` once and otherwise
  treats it as infrastructure.

## The one rule that matters most

**Always run build/toolchain commands through `nix develop -c <cmd>`** — never
invoke `idf.py`/`cmake`/`esptool` directly.

```sh
nix develop -c ./run.sh                                             # host simulator
nix develop -c ./run.sh esp32p4                                     # flash + monitor
nix develop -c ./run.sh esp32p4 build
nix develop -c ./run.sh simulator --verify simulator/verify/home.txt # headless, writes captures/
nix develop -c ./run.sh esp32p4 --verify <port> simulator/verify/home.txt
```

The flake only sees files git knows about: a new file must be at least
`git add`ed before `nix develop` picks it up.
