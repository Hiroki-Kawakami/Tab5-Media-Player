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

## Audio decoder memory

`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=n` keeps the P4's 32 KiB LP/RTC
SRAM out of the heap. It is reachable from the HP core but uncached and behind
the LP bus, so it is far slower than PSRAM, which is cached: once internal SRAM
ran short, `esp_audio_codec` picked up ~25 KiB of MP3 working buffers there and
decoding one frame went from 3 ms to 37 ms — 26 cycles per instruction, below
real time, which dragged the whole timeline down because the media clock
follows the audio position. Anything that quietly lands in that pool has the
same failure mode, so the pool is gone rather than steered around.

The `media_audio` task therefore only takes the stack its codec needs: 4 KiB
covers MP3/AAC/ADPCM (measured: under 1 KiB of use), and the task is recreated
with 20 KiB for Opus, which spends 11 KiB. The host port ignores the stack size,
so it keeps the task it already has.

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

Rotation Lock (Home > Display) freezes the UI at the rotation in effect when it
is switched on, and stops the IMU polling with
`bsp_imu_set_orientation_enabled(false)` for as long as it holds; that is the
only thing the lock buys over ignoring the callback. `settings.cpp` owns the
state and `settings_set_rotation_lock` is the single entry point, so a second
place to toggle it only has to call that; `ui_orientation` keeps a copy to drop
callbacks that are already in flight when tracking stops. One NVS byte carries
both halves (0 unlocked, otherwise the rotation plus one). The saved rotation is
applied in `ui_orientation_start` before Home is created, so a locked boot lays
Home out once instead of rotating it afterwards.

## Player overlay

The bars and the two panels (Settings and Media Info) are LVGL objects on one
full-screen display of its own, created when the player opens. It cannot be the
main display: during playback the main display's draw buffers are the SRAM the
decoder took (see [Shared SRAM buffer](#shared-sram-buffer)), so LVGL has
nowhere to render.
`buffer.lines` is kept small because only the bars are ever invalidated; the
default of a quarter screen would be idle PSRAM.

Video and UI share framebuffer 0 — with UI insets set the presenter stops
rotating through the three framebuffers and clips itself out of the UI area,
and LVGL blits the bars into that same buffer. Every pixel belongs to exactly
one of the two, which holds as long as LVGL never invalidates the video area:

- LVGL joins two dirty areas only where they overlap, so the top and the bottom
  bar cannot merge into a full-screen repaint.
- The full-screen object that catches taps on the video carries no styles, so
  pressing it changes no style state and invalidates nothing.
- Anything that invalidates the whole screen paints black over the video. The
  display is created before `video_presenter_begin()` and `onEnter` pushes that
  first pass out with `lv_refr_now` before `player_open`, so it cannot land on
  top of a frame: the board is slow enough to show the frame first, blank it
  and only then play. The only such event left during playback is the
  resolution change inside `set_rotation`, which
  `PlayerScreen::rotate` suppresses with `lv_display_enable_invalidation` and
  replaces with an invalidate of the bars alone.
- Building a bar invalidates wherever it sits until the layout runs, which for
  an aligned bar is the top-left corner, so `buildUi` settles the layout while
  those invalidations are still being dropped. Without that the video is left
  with a black band the size of a bar, visible until the next frame overwrites
  it — which, paused, never comes.

Switching between the bars, a panel and nothing at all is then plain LVGL plus
a clip change, in this order: hide what is leaving, `lv_refr_now` and
`bsp_display_wait_draw` so it is painted out, move the insets, and only then
show what is arriving. Either half in the other order lets the video draw over
the UI, or leaves the UI's last pixels sitting in the video area.

The Settings panel holds the settings that can be changed mid-playback: Color
Mode is not one of them, because reconfiguring the panel format tears the video
path down. Bars and panels are never up at the same time, so the volume they
both show is read again when one of them is shown rather than kept in sync.

Media Info takes the same area as Settings, so both are one case in the inset
logic. Its rows come from a `MediaSummary` the player freezes when the file
opens, and which rows exist at all depends on the file, so it is rebuilt every
time it is shown instead of being refreshed in place. It is also the only part
of the overlay that scrolls: three sections do not fit the panel on either
orientation.

The contents are taller than the panel (828 px against 560 in portrait, 640 in
landscape), and repainting that much text per scroll step is beyond what the
board can do into a PSRAM draw buffer. So they are rendered once, on a
throwaway offscreen display, into an RGB565 image in PSRAM (around 1.2 MB,
freed with the image object), and the scroll moves that image: every frame
after the first is a copy instead of fills, rounded rects and glyph blending.
Nothing in the panel changes while it is open, so one render is enough. If the
allocation fails the widgets are simply left in place.

Scrolling it is still the one place where the UI and the video are both busy at
once, and on the board neither keeps up: the scroll crawls and playback
stalls. So a
scroll suspends the video — `player_suspend_video()` drops video packets as
they come due instead of decoding them, which leaves the reader pacing the file
as before and the audio playing, and the picture picks up at the next keyframe
when the scroll ends. It is cleared whenever the panel is built or left, since
a panel torn down mid-scroll never sends the closing event.

## Home screen

`HomeScreen` is the only ScreenManager screen besides `PlayerScreen`. The menu
(SD Card, USB Drive, Display, Sound) and the settings/file browser pages are not
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

`settings_init()` only opens NVS and reads the values, so `app_entry()` can call
it before `bsp_init()`: the display pixel format is part of `bsp_config` and has
to be known by then. Everything the BSP does not take through `bsp_config` is
pushed by `settings_apply()` right after `bsp_init()`: brightness, volume, the
equalizer, and the headphone callback.

The settings pages share their rows, sliders and segmented toggles through
`app/screens/home/settings_widgets.*`; a page is then only its values and the
side effects of changing them.

Volume is stored twice, one value per output route: the speaker and a pair of
headphones are comfortable at very different settings, so a single value means
re-dragging the slider on every insert and removal. The Sound page shows one
slider that follows the route instead — `settings_volume()` reads the value for
the route the BSP reports, and the headphone callback applies the other one on
insert and removal. That callback runs on the BSP dispatch task, so the UI is
told through `settings_volume_observe()`, whose observers are dispatched on the
LVGL thread and released with the object they were registered on. `bsp_audio_set_mute()` is
what the player's mute button toggles — muting by setting the volume to zero
would persist the zero.

The Display page's Color Mode switches the panel between RGB565 and RGB888
without a restart. `media_player_set_display_pixel_format()` hides the main
display, waits for the blit in flight, calls `bsp_display_reconfigure()` and
rebinds LVGL with `display_manager.set_color_format()`; showing the display
again is what repaints it, because a hidden display drops invalidations. It runs
from `lv_async_call`, not from the click, so the panel is not torn down inside an
LVGL event. The default is RGB565: the panel format is also what the video path
writes into the framebuffers, and 16-bit halves those bytes. A failed switch
leaves the previous format up (the driver restarts the panel with it), so the UI
puts the segment back and says so.

RGB565 needs the PPA fix that `esp-devkit/flake.nix` patches into ESP-IDF: on
v6.1 a rotated SRM blit whose leftover block is smaller than the 2D-DMA FIFO
never raises its completion interrupt, which wedges the whole PPA client. The
landscape UI hits it at 16-bit (a 879x69 chunk) but not at 24-bit (879x46).

Video renderers read the panel format once, when the player opens
(`video_presenter_begin()`), which is fine because Color Mode lives in Home and
the player cannot be open at the same time.

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
`SIMULATOR_NVS_PATH` to `simulator/nvs_data.json`. It outlives the process, so
whatever a verify script changes is still there for the next run; the rotation
lock in particular has to be switched back off at the end, or every later script
starts frozen in that orientation.

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

`components/usb_msc` drives one drive on the Tab5's USB-A port, mounted at
`/usb` by the Home screen's USB Drive button like the SD card. It lives here
rather than in esp-devkit because a shared USB host library there should cover
more than MSC. `msc_bot.c` implements Bulk-Only Transport and the SCSI subset a
drive needs (INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10),
READ10/WRITE10) on top of the IDF host library, and `usb_msc.c` hands the
result to FatFS through `ff_diskio_register`.

The in-house transport replaced `usb_host_msc` because that component copies
every data phase through its own transfer buffer. With the buffers in PSRAM
(`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM`) a 64 KB read touched PSRAM
three times: DMA in, then a CPU copy out. During 720x1280 playback, where the
JPEG decoder and the panel scanout already saturate PSRAM, that copy cost about
31% of each read and held throughput near what a 32 Mbps stream needs, so heavy
scenes starved the reader. Its single transfer buffer also grew to the largest
read FAT asked for, which made every 31-byte command sync a 64 KB cache range,
and a failed regrow left a dangling pointer that the next command freed again
(`usb_host_msc` 1.3.0).

`data_stage()` avoids the copy by pointing the transfer at the caller's buffer:
`urb_alloc()` only assigns `data_buffer`/`data_buffer_size` after allocating
them separately, and `hcd_dwc.c`'s `cache_sync_data_buffer()` documents that
class drivers may overwrite those fields. An IN transfer is synced with
`ESP_CACHE_MSYNC_FLAG_DIR_M2C`, which has no unaligned path, so a borrowed
buffer must be cache aligned in both address and size, and `num_bytes` must be
a multiple of the endpoint's max packet size; anything else (FatFS's own window
buffer) falls back to a bounce buffer. `media_buffer`'s chunks are 64 KB
aligned, so playback reads always borrow. CBW and CSW ride a separate 512-byte
transfer.

Measured against `usb_host_msc` on the same drive and file: sequential 64 KB
reads 12.9 -> 16.2 MB/s idle, and during 60 fps 720x1280 playback a 64 KB
`media_buffer` chunk takes 10.2 ms instead of 14.4 ms, which is enough headroom
to hold 50 fps where the old path dropped to 36 fps with bursts of 23 dropped
frames in a row.

The host stack is installed at boot so a drive plugged in later is seen. The
device is opened as soon as it enumerates, not at mount time, so a drive pulled
out before mounting is still noticed. The open runs on the component's own
worker task: transfers complete on the client task, so opening from the client
event callback would deadlock.

Pulling a mounted drive does not unmount it. The player may still hold a file
open, and FAT must not be unregistered under an open fd; the disconnect sends
`player_eject("/usb")`, which stops reading at once, then closes a player on a
`/usb` file and drops the `/usb` pages from Home. The
stale mount is released by the next `usb_msc_mount`, which runs from Home after
the player and the `/usb` pages are gone. A drive plugged in while a stale mount is held
is opened at that point too.

The host stack and the component's three tasks take about 25 KB of internal RAM
at boot.

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
