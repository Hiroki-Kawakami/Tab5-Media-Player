# Architecture

## Layout

| path | what it is |
|---|---|
| `app/` | the firmware, shared verbatim by both targets (device + host simulator) |
| `components/` | project-specific plain-C components (`media_buffer`, `avi_demux`, `mkv_demux`, `h264_dec`) |
| `esp32p4/` | ESP-IDF wrapper for the Tab5: sdkconfig, partition table, `app_main` |
| `simulator/` | host wrapper: SDL/host `main`, its own sdkconfig |
| `simulator/verify/` | harness scripts for headless UI checks |
| `esp-devkit/` | submodule: BSP, LVGL port, `ui_framework`, harness |

Everything shared lives in esp-devkit and is consumed through `devkit.cmake`
(`devkit_idf_init` / `devkit_simulator`). Reusable board/simulator/UI
infrastructure is advanced in esp-devkit first, then this repo bumps the
submodule pointer. Project-specific components go under a top-level
`components/` and are added to both wrappers' `COMPONENT_DIRS`.

`app/CMakeLists.txt` globs its sources with `CONFIGURE_DEPENDS`: the
simulator's Ninja build otherwise never re-scans the tree, and a newly added
`.cpp` fails at link time until someone reconfigures by hand (`idf.py` always
reconfigures, so the device build hides this). The flag is dropped under
`CMAKE_SCRIPT_MODE_FILE` because ESP-IDF's requirement scan includes component
CMakeLists in script mode, where it is a hard error.

`app_entry()` in `app/media_player.cpp` is the single entry point for both
targets: `bsp_init()`, the esp-devkit LVGL port, `display_manager.create_display()`,
then the first screen. `esp32p4/main/main.cpp` and `simulator/main/main.cpp` only
call it.

The media pipeline (containers, decode, presentation, audio, and the extension
points left for MKV/H.264/playlists) is described in [`playback.md`](playback.md).

## Shared SRAM buffer

`app/media_player.cpp` reserves one 245760-byte, 64-byte-aligned array in
`.bss`, which lands in internal SRAM (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`
is off). It is split into two halves that serve two owners in turn:

- the main LVGL display renders in `DisplayRenderMode::Partial` with the halves
  as its two draw buffers;
- MJPEG playback uses them as `jpeg_decode_enhanced` strip buffers;
- H.264 playback uses the whole block as the decoder's work arena
  (`SharedSram::base`/`bytes`, see [`h264.md`](h264.md#memory)).

A hidden display does not render, so the halves are free while the main display
is hidden. `media_player_acquire_sram()` hides it and waits for
`bsp_display_wait_draw()`, because the second buffer makes partial blits
asynchronous and one may still be reading a half. `media_player_release_sram()`
shows it again. Anything else that wants the halves goes through the same pair,
which also means media that keeps the LVGL main UI on screen cannot use them.

The size is two strips of 16 rows × 2560 px × 3 bytes, which is where the 2560 px
width limit for MJPEG comes from. The same 240 KiB is also what caps H.264 at
1280 px wide.

Tasks that run PIE code get their stacks from `MALLOC_CAP_SIMD` alone
(`h264_create_task()`): this build lets the heap use RTC RAM, and a PIE task
whose stack lands there hangs the chip when FreeRTOS saves its vector
registers. Adding `MALLOC_CAP_INTERNAL` looks harmless but leaves almost no
matching memory by the time the player opens (details in
[`h264.md`](h264.md#pie)). If `video_presenter_begin()` fails anyway,
`PlayerScreen` gives the SRAM back and says so in a modal.

## Media arena

`app_entry()` also allocates the 4 MB PSRAM arena that playback reads into
(see [`playback.md`](playback.md)). It is allocated at boot and never freed
because it must be one contiguous block: allocating it per session would risk
PSRAM fragmentation making it unavailable after the UI has been used for a
while. It cannot be a `.bss` array like the SRAM buffer, because `.bss` stays in
internal RAM.

## Orientation

`app/ui_orientation.cpp` follows `bsp_imu_get_orientation()` for all four
rotations; `UNKNOWN` and `FACE_UP/DOWN` keep the current one. With no listener
it rotates the main display with `display_manager.set_rotation`, which the
Partial render mode supports and which swaps the LVGL resolution. Home and the
file browser have no landscape layout; their portrait layout just stretches.

While the player is open it registers itself as the listener and the main
display is left alone. `set_rotation` re-hands the draw buffers to LVGL, and
during playback those buffers are the decoder's. The main display catches up
when the listener is cleared, which `PlayerScreen::onExit` does after the
decoder is gone and before the display is shown again.

Opening the player keeps whatever rotation the UI already has; the video's
aspect does not choose it.

## SD card

The card is mounted at `/sdcard` when the Home screen's SD Card button is
pressed, not at boot, so a card inserted after power-on still works and a
missing card surfaces as a modal rather than a log line. It is never unmounted.

`FileBrowserScreen` classifies entries by `dirent::d_type` rather than `stat`:
on FAT every `stat` is another directory scan, which adds up on a folder of
hundreds of files. Entries starting with `.` are skipped, which also hides the
`._*` AppleDouble files macOS leaves on cards.

On the simulator `bsp_sd_mount` redirects `/sdcard` to a host directory
(`esp-devkit/bsp/simulator/sd_redirect.c`). Its default is relative to the
process cwd, and the harness launches the simulator from wherever `run.sh` was
invoked, so `run.sh` pins `SIMULATOR_SDCARD_PATH` to `simulator/sdcard`
(gitignored — put test media there).

Row labels use the Montserrat fonts, which have no CJK glyphs: non-ASCII file
names render as missing-glyph boxes until a font covering them is loaded.

## Harness

`CONFIG_HARNESS=y` is set for the device build (the simulator always has it),
so `./run.sh simulator --verify` / `./run.sh esp32p4 --verify` can inject touches and read the panel
back as JPEG. The console is USB-Serial-JTAG, which is what keeps full-panel
captures fast. The harness links the JPEG encoder, which is why the factory
partition is 4M rather than `singleapp`'s default. Coordinates in scripts are
panel pixels (720x1280 portrait) whatever the UI rotation. Scripts that need
landscape inject it with `imu rot90`; the headless simulator otherwise stays at
rotation 0. See `esp-devkit/docs/harness.md`.

Opening the USB-Serial-JTAG port resets the board, so every
`./run.sh esp32p4 --verify` starts from a fresh boot on the Home screen. A
script cannot pick up where a previous run left off; drive the whole path from
Home in one script.
