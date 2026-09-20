# Architecture

## Layout

| path | what it is |
|---|---|
| `app/` | the firmware, shared verbatim by both targets (device + host simulator) |
| `components/` | project-specific plain-C components (`media_buffer`, `avi_demux`, `mkv_demux`, `mp4_demux`, `vdec_common`, `h264_dec`, `mpeg2_dec`, `usb_msc`) |
| `esp32p4/` | ESP-IDF wrapper for the Tab5: sdkconfig, partition table, `app_main` |
| `simulator/` | host wrapper: SDL/host `main`, its own sdkconfig |
| `simulator/verify/` | harness scripts for headless UI checks |
| `esp-devkit/` | submodule: BSP, LVGL port, `ui_framework`, harness |
| `tools/resgen/` | build-time generator for LVGL fonts, icon fonts and images (see [`resources.md`](resources.md)) |
| `tools/converter-rs/` | host CLI, desktop app and browser version that convert videos for the player (see [`converter.md`](converter.md)) |

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
- H.264 and MPEG-2 playback use the whole block as the decoder's work arena
  (`SharedSram::base`/`bytes`, see [`h264.md`](h264.md#memory) and
  [`mpeg2.md`](mpeg2.md#memory)).

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
(`video_create_task()`): this build lets the heap use RTC RAM, and a PIE task
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
Partial render mode supports and which swaps the LVGL resolution. Home has its
own landscape layout (see [Home screen](#home-screen)); the player handles
rotation itself.

While the player is open it registers itself as the listener and the main
display is left alone. `set_rotation` re-hands the draw buffers to LVGL, and
during playback those buffers are the decoder's. The main display catches up
when the listener is cleared, which `PlayerScreen::onExit` does after the
decoder is gone and before the display is shown again.

Opening the player keeps whatever rotation the UI already has; the video's
aspect does not choose it.

## Home screen

`HomeScreen` is the only ScreenManager screen besides `PlayerScreen`. The menu
(SD Card, USB Drive, Display) and the settings/file browser pages are not
separate screens but a page stack inside it (`app/screens/home/`), because
landscape shows both at once: the menu on the left, the top page on the right.
Portrait shows either the menu (empty stack) or the top page full-screen, which
is the same navigation the separate screens used to give.

Every navigation, rotation and eject rebuilds the whole view on the next LVGL
tick rather than in place. Navigation is triggered from a click on a row or
back button that the rebuild deletes, and the list calls back into the page
that a pop destroys, so neither can happen inside the event. Pages outlive
their views: a `FileBrowserPage` keeps its entries and scroll offset, so going
back or rotating does not re-read the directory.

Settings pages are pages of that same stack, so a setting opens next to the
menu in landscape like a folder does. `app/settings.cpp` owns the values and
keeps a single NVS handle open for the run, rather than an open/close around
every setting. A setter applies the change to the hardware immediately and only
marks the entry modified; `settings_commit()` writes the marked entries and is
called when an interaction ends: the brightness slider commits on
`LV_EVENT_RELEASED`, not on every `LV_EVENT_VALUE_CHANGED`, so a drag costs one
flash write instead of one per step.

A setting is a `Setting<"key", T, sanitize>` object. Key and sanitizer live in
the type, so the object in RAM is the value plus a modified flag and nothing
else: brightness is two bytes. With the video path short of SRAM, a settings
list that grows to dozens of entries should stay in that order.

`Codec<T>` maps the value type to its `nvs_get_*`/`nvs_set_*` pair, so each
setting is stored at its own width (brightness as `u8`, not `i32`) and a type
the machinery has not seen is one specialization; `std::string` is there
already, because credentials and paths are the obvious next settings. The
sanitizer is the one place a range is written: it clamps both what the UI sets
and what comes back out of NVS, where a value may be stale or garbage.
`for_each_setting` is the list `settings_init` and `settings_commit` walk; a
setting missing from it simply never persists.

Committing untouched entries would not write, but it is not free either: NVS
compares against flash, which costs two 32-byte reads per entry, each with the
cache disabled on both cores. The per-entry modified flag keeps a commit at
zero flash access when nothing changed. Without an NVS partition the settings
still work for the session and only the write is skipped.

On the simulator NVS is esp-devkit's JSON-file store. Its default is relative
to the process cwd like the SD card redirect, so `run.sh` pins
`SIMULATOR_NVS_PATH` to `simulator/nvs_data.json`.

Landscape is decided from the Home root's size, not `ui_orientation_current()`:
while the player is open the IMU rotation moves on but the main display does
not, and Home only relayouts when the display actually changes.

A USB disconnect removes every page under `/usb` from the stack, and closes the
player if it is playing from there.

## SD card

The card is mounted at `/sdcard` when the Home screen's SD Card button is
pressed, not at boot, so a card inserted after power-on still works and a
missing card surfaces as a modal rather than a log line. It is never unmounted.

`FileBrowserPage` classifies entries by `dirent::d_type` rather than `stat`:
on FAT every `stat` is another directory scan, which adds up on a folder of
hundreds of files. Entries starting with `.` are skipped, which also hides the
`._*` AppleDouble files macOS leaves on cards.

The card is mounted with `psram_bounce_buffer`. Reads into a buffer that is not
cache-line aligned (an MP4 index at an arbitrary file offset, for one) go
through a bounce buffer, which IDF otherwise mallocs from internal DMA memory
per read; during playback there is less than a sector of it left, and the read
fails with `allocate_dma_buf: not enough mem`. The P4's SDMMC can DMA into
PSRAM, so the BSP hands it one PSRAM buffer for the whole mount instead.

On the simulator `bsp_sd_mount` redirects `/sdcard` to a host directory
through esp-devkit's `simulator/path_redirect.h`. Its default is relative to the
process cwd, and the harness launches the simulator from wherever `run.sh` was
invoked, so `run.sh` pins `SIMULATOR_SDCARD_PATH` to `simulator/sdcard`
(gitignored — put test media there).

Row labels use the Montserrat fonts, which have no CJK glyphs: non-ASCII file
names render as missing-glyph boxes until a font covering them is loaded.

## USB drive

`components/usb_msc` wraps the USB host library and `usb_host_msc` for one
drive on the Tab5's USB-A port, mounted at `/usb` by the Home screen's USB Drive
button like the SD card. It lives here rather than in esp-devkit because a
shared USB host library there should cover more than MSC.

The host stack is installed at boot so a drive plugged in later is seen. The
driver is installed as soon as the drive enumerates, not at mount time:
`usb_host_msc` only reports `MSC_DEVICE_DISCONNECTED` for installed devices, so
an unmounted drive pulled out would otherwise go unnoticed. The install runs on
the component's own task because `msc_host_install_device` needs the MSC
background task to process events and would deadlock inside its callback.

Pulling a mounted drive does not unmount it. The player may still hold a file
open, and FAT must not be unregistered under an open fd; the disconnect sends
`player_eject("/usb")`, which stops reading at once, then closes a player on a
`/usb` file and drops the `/usb` pages from Home. The
stale mount is released by the next `usb_msc_mount`, which runs from Home after
the player and the `/usb` pages are gone. A drive plugged in while a stale mount is held
is installed at that point too.

`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` puts the USB transfer buffers in
PSRAM. `usb_host_msc` grows its single transfer buffer to the largest read FAT
asks for (64 KB chunks from `media_buffer`, more for an MP4 index), which
internal DMA memory cannot supply during playback. When that allocation fails,
`msc_bulk_transfer` has already freed the old buffer and leaves the dangling
pointer in place; the next command frees it again and the heap asserts
(`usb_host_msc` 1.3.0).

The host stack, its two tasks here and the MSC driver's task take about
12.5 KB of internal RAM at boot.

On the simulator the drive is `SIMULATOR_USB_PATH` (`run.sh` pins it to
`simulator/usb`, gitignored), attached at boot when that directory exists.
Harness scripts plug and pull it with `usb-attach [dir]` / `usb-detach`; see
`simulator/verify/usb.txt`. Those commands exist only on the simulator.

Adding `usb_host_msc` made the component manager re-solve
`esp32p4/dependencies.lock`, which moved LVGL to a 9.6 pre-release whose
`lv_conf_internal.h` fails the build with `-Werror`. The lock keeps LVGL at
9.5.0; watch for that bump whenever a managed dependency is added.

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
