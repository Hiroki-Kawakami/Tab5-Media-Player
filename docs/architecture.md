# Architecture

## Layout

| path | what it is |
|---|---|
| `app/` | the firmware, shared verbatim by both targets (device + host simulator) |
| `components/` | project-specific components (`media_buffer`, `media_tags`, `riff_demux`, `es_audio_demux`, `mkv_demux`, `mp4_demux`, `vdec_common`, `h264_dec`, `mpeg2_dec` in plain C; `airplay` in C++) |
| `esp32p4/` | ESP-IDF wrapper for the Tab5: sdkconfig, partition table, `app_main` |
| `esp32p4oc/` | the same wrapper overclocked (CPU 400 MHz, PSRAM 220 MHz); see [Overclocked build](#overclocked-build) |
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
The image viewer, which shares the decoders but none of the playback machinery,
is described in [`images.md`](images.md).

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
`VideoPlayerScreen` gives the SRAM back and says so in a modal.

The video decoders are no longer the only PIE code: image_framework decodes
JPEG and resizes with it too, which puts the same rule on the metadata worker's
decoder task and on the buffers it hands the decoder
([`metadata.md`](metadata.md#nothing-of-this-lives-in-internal-ram)).

## Media arena

`app_entry()` also allocates the 4 MB PSRAM arena that playback reads into
(see [`playback.md`](playback.md)). It is allocated at boot and never freed
because it must be one contiguous block: allocating it per session would risk
PSRAM fragmentation making it unavailable after the UI has been used for a
while. It cannot be a `.bss` array like the SRAM buffer, because `.bss` stays in
internal RAM.

The Video Input screen borrows the same arena for camera frames while it is
open (see [Video input](#video-input)); `player_arena()` hands it out between
`player_close()` and the next `player_open()`.

A second, 512 KB arena is allocated next to it for the metadata worker, which
opens files for their tags and cover art while the player holds the big one
(see [`metadata.md`](metadata.md)).

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
covers MP3/ADPCM (measured: under 1 KiB of use) and AAC (`audf_aac`, under 2 KiB
including a bench task's own frames), and the task is recreated
with 20 KiB for Opus, which spends 11 KiB. The host port ignores the stack size,
so it keeps the task it already has.

## Orientation

`app/ui_orientation.cpp` follows `bsp_imu_get_orientation()` for all four
rotations; `UNKNOWN` and `FACE_UP/DOWN` keep the current one. With no listener
it rotates the main display with `display_manager.set_rotation`, which the
Partial render mode supports and which swaps the LVGL resolution. Home has its
own landscape layout (see [Home screen](#home-screen)); the video player handles
rotation itself. `AudioPlayerScreen` keeps the main display, so it is rotated
for it and only relays out its own contents on `LV_EVENT_SIZE_CHANGED`, like
Home.

While the video player is open it registers itself as the listener and the main
display is left alone. `set_rotation` re-hands the draw buffers to LVGL, and
during playback those buffers are the decoder's. The main display catches up
when the listener is cleared, which `VideoPlayerScreen::onExit` does after the
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
the rotation and a lock bit. Unlocked, a rotation is written only after the UI
has held it for 5 s, so turning the tablet around does not write NVS on every
step. The saved rotation is applied in `ui_orientation_start` before Home is
created, so a boot lays Home out once instead of rotating it afterwards; if the
IMU then reports a different pose, that pose wins.

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
  `VideoPlayerScreen::rotate` suppresses with `lv_display_enable_invalidation` and
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
path down. It takes a mask of the sections to build, which is how the image
viewer shows the same Display rows without the Sound ones
(see [`images.md`](images.md)); the panel shell and the info rows it shares with
the video player live in `app/screens/player_panel.*`. Bars and panels are never up at the same time, so the volume they
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

Even cached, the panel and the video are both busy at once on the board and
neither keeps up: the scroll crawls and playback stalls. Suspending only the
decoding while the scroll runs was not enough — the reader and the audio keep
the memory bus busy, and the picture still broke up. So the panel plays over a
stopped picture instead: `setMode()` pauses playback when Media Info opens, and
plays again when it closes, but only if it was playing when the panel opened.
Paused, the player task blocks on its command queue, which leaves the whole
bus to the UI. Pausing before the insets move matters: `handle_repaint()`
ignores a repaint while playing, so it is the pause that lets the still picture
be redrawn into the area the panel leaves it.

## Home screen

`HomeScreen` is one of four ScreenManager screens, next to `VideoPlayerScreen`,
`AudioPlayerScreen` (see [`playback.md`](playback.md#the-audio-screen)) and
`ImageViewerScreen` (see [`images.md`](images.md)). The menu
(SD Card, USB Drive, Display, Sound) and the settings/file browser pages are not
separate screens but a page stack inside it (`app/screens/home/`), because
landscape shows both at once: the menu on the left, the top page on the right.
Portrait shows either the menu (empty stack) or the top page full-screen, which
is the same navigation the separate screens used to give.

The menu pane and the page pane are built once; rotation only resizes and hides
them. Every navigation, rotation and eject rebuilds the page pane on the next
LVGL tick rather than in place. Navigation is triggered from a back button that
the rebuild deletes, and the list calls back into the page that a pop destroys,
so neither can happen inside the event. Pages outlive their views: a
`FileBrowserPage` keeps its entries and scroll offset, so going back or
rotating does not re-read the directory.

The menu is never rebuilt by navigation, so selecting a row in landscape keeps
its scroll position. `refresh_menu()` rebuilds only the rows, for when the set
of items changes; it keeps the scroll offset and closes the page of a selected
item that went away. The selection is kept as a `MenuId`, not a row index, so
it survives items appearing above it.

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
pushed by `settings_apply()` right after `bsp_init()`: brightness and the
equalizer. Volume, mute and the headphone callback belong to
`audio_output_init()`, which runs right after it.

The settings pages share their rows, sliders and segmented toggles through
`app/screens/home/settings_widgets.*`; a page is then only its values and the
side effects of changing them.

Volume is stored once per output route — speaker, headphones, and one value
shared by every USB audio device: they are comfortable at very different
settings, so a single value means re-dragging the slider on every insert and
removal. The Sound page shows one slider that follows the route instead.
`app/audio/audio_output.*` owns the route: USB while a device is connected,
otherwise what the BSP's headphone detect reports. `settings.cpp` only stores the
three values. Route changes arrive on the BSP dispatch task and the USB worker
task, so the UI is told through `audio_output_volume_observe()`, whose observers
are dispatched on the LVGL thread and released with the object they were
registered on. `audio_output_set_mute()` is what the player's mute button
toggles — muting by setting the volume to zero would persist the zero. AirPlay's
sender volume goes through `audio_output_apply_volume()`, which is not stored
and lasts until the route changes.

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

## Overclocked build

`esp32p4oc/` builds the same firmware with the CPU at 400 MHz and PSRAM at
220 MHz from boot; `esp32p4/` stays at the 360/200 MHz Espressif guarantees for
rev 1.x. It shares `main`, `sdkconfig.defaults`, the partition table and
`dependencies.lock` with `esp32p4/` and only adds the two overrides. The gain
did not justify a switch in the UI: H.264 decode got about 6% faster from the
CPU, and PSRAM at 240 MHz took about 3% more off on top of that.

Both clocks need esp-devkit's IDF patches. v6.1 lets Kconfig pick a 400 MHz CPU
on rev 1.x but `rtc_clk.c` rejects it and aborts at boot. `CONFIG_SPIRAM_SPEED_220M`
is the 250 MHz latency settings with MPLL at 440 MHz. At 250 MHz rev 1.x returns
bursts with one byte lane a beat off and stray address bits on writes, and no
DQS phase, delay line, read dummy or drive strength setting changes the rate.
240 MHz runs on some chips and corrupts PSRAM on others, rev 1.3 included;
220 MHz runs on every board at hand. A hash-checked decode is what catches it:
the boot-time memory test and the IDF tuning pattern both pass at 250 MHz.

After an IDF patch changes the IDF store path, an existing `build/bootloader`
CMake cache still points at the old path and the configure step fails; delete
that directory (or `fullclean`). `flake.lock` pins esp-devkit by commit, so a
patch takes effect only after esp-devkit is committed and
`nix flake update esp-devkit` is run.

## CI build

`.github/workflows/firmware.yml` (run by hand or on a `v*` tag) builds
`esp32p4/` and `esp32p4oc/` with esp-devkit's CI actions
(`esp-devkit/docs/ci.md`) and uploads the merged binary for M5Burner, the ELF
and the sdkconfig per target. A tag also publishes a release of that name with
only the two merged binaries attached. The image adds `RESGEN_PYTHON`; its tag
hashes the whole `flake.nix`, so devShell-only edits there rebuild it too. CI
sees only committed state: `sdkconfig` comes from `sdkconfig.defaults`, and
esp-devkit changes need `nix flake update esp-devkit` as for local builds.

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

The USB-A port is driven by esp-devkit's `libs/usb_host`, its own host stack
on IDF's DWC2 HAL rather than `espressif/usb`, with `CONFIG_USBH_MSC`; its
README covers the controller, the transport, why `usb_host_msc` is not used,
and how a mount outlives its drive. It does not use `espressif/usb`: some
capture devices never answer after the second bus reset its enumeration sends,
and its isochronous scheduling stops the channel between transfers. The drive is mounted at `/usb` by the
Home screen's USB Drive button like the SD card. `app_entry()` switches VBUS on
and installs the host stack at boot so a drive plugged in later is seen.

`app_entry()` keeps the first connected `MscDevice` and drops it on its
disconnect. The USB Drive row in the Home menu and in the BGM picker's storage
list exists only while it is held; both lists are rebuilt from the connect and
disconnect callbacks. Pulling a mounted drive sends `player_eject("/usb")`, which stops
reading at once, then closes a player on a `/usb` file and drops the `/usb`
pages from Home. The mount itself stays until the next
`media_player_mount_usb()`, which runs from Home after the player and the
`/usb` pages are gone and unmounts it before mounting the current drive.

Playback reads borrow `media_buffer`'s chunks directly, since those are 64 KB
aligned. Measured against `usb_host_msc` on the same drive and file:
sequential 64 KB reads 12.9 -> 16.2 MB/s idle, and during 60 fps 720x1280
playback, where the JPEG decoder and the panel scanout already saturate PSRAM,
a 64 KB chunk takes 10.2 ms instead of 14.4 ms. That holds 50 fps where the
copying path dropped to 36 fps with bursts of 23 dropped frames in a row.

The host stack and its two tasks take about 25 KB of internal RAM at boot.

On the simulator the drive is `SIMULATOR_USBH_MSC_PATH` (`run.sh` pins it to
`simulator/usb`, gitignored), attached at boot when that directory exists.
Harness scripts plug and pull it with `usbh-msc-attach [dir]` /
`usbh-msc-detach`; see `simulator/verify/usb.txt`. Those commands exist only
on the simulator.

Adding `usb_host_msc` made the component manager re-solve
`esp32p4/dependencies.lock`, which moved LVGL to a 9.6 pre-release whose
`lv_conf_internal.h` fails the build with `-Werror`. The lock keeps LVGL at
9.5.0; watch for that bump whenever a managed dependency is added.

## USB audio

A USB audio device on the USB-A port takes the output over while it is
connected (`CONFIG_USBH_UAC`; esp-devkit's `libs/usb_host` README covers what
the driver supports). `app/audio/audio_output.*` is the one output every
player writes to — `audio_decoder` and the AirPlay receiver call
`audio_output_open/write/close`, never `bsp_audio_*` — and it moves an open
stream between the board and the device when one is plugged in or pulled.
Only the first device that connects is used.

The board's DSP, EQ and software gain stay on the board's path: they correct
the built-in speaker and headphone amp, and have nothing to do with an
external DAC. On the device, volume and mute go to its feature unit, and the
slider spans the whole range the device reports, linear in dB from its minimum
at 1 to its maximum at 100, with 0 muting: DACs differ too much in output level
for a fixed dB curve to leave the slider usable on all of them. A device without
a volume control gets a software gain on the board's curve instead (1..100 is
-40..0 dB), and one without a mute control is muted through that gain.

`app/audio/usb_audio_output.*` picks the device format that matches the
stream's channel count with the closest bit depth, and builds an
esp-devkit `audio_framework` graph for whatever differs: channel mixing,
resampling, the software gain, and the sample width at the sink. A stream that
needs none of it goes to the device untouched.

Resampling happens only when the device does not list the stream's rate. The
target is a rate an integer multiple or divisor away when the device has one,
which takes the cheaper `INTEGER` resampler, and otherwise the lowest rate
above the stream's (the highest below it as a last resort). A non-integer
ratio takes `POLYPHASE` for music and AirPlay and Catmull-Rom (`CUBIC`) for
video; `audio_output_open()` carries which of the two a stream is. Everything
the graph allocates comes from PSRAM through the modules' `alloc_caps`. A
stream the graph cannot be built for is logged and consumed in real time as
silence, which keeps the player's clock running.

Hubs are not supported, so a drive, a USB audio device and a camera cannot be
used at the same time.

On the simulator, `usbh-uac-attach [wav] [rate,...]` / `usbh-uac-detach` plug
and pull a device that records to a WAV file; see
`simulator/verify/usb_audio.txt`.

## Video input

A USB camera or capture device (`CONFIG_USBH_UVC`, esp-devkit's
`libs/usb_host` README) adds Video Input to the Home menu's Device section
while it is connected; `app_entry()` keeps the first one, like the drive.
`VideoInputScreen` is pushed full screen like the AirPlay receiver, not as a
Home page, and goes back on its own when the camera is pulled.

It is the video player without the player: the same `video_presenter` and
`MjpegRenderer`, the shared SRAM as strip buffers, the same overlay display and
inset handover (see [Player overlay](#player-overlay) and
[`playback.md`](playback.md#the-overlay)), with frames submitted as they arrive
(`due_us` 0). The bars are 80 px top and bottom, start hidden, and hide again
after 4 s. MJPEG cannot redraw a frame it has drawn, and nothing pins the last
packet the way the player does, so a mode change waits for the next frame to
fill the area the UI left; while a camera sends nothing, that area keeps
whatever was there.

The top bar's right end shows the input format, and opens the Input Format
panel: Resolution and Frame Rate dropdowns over the camera's MJPEG sizes of at
most 1920x1080 pixels that `MjpegRenderer::fits`, at up to 60 Hz (a continuous
interval range is offered as the common rates inside it). The last choice is
one setting, not one per device; a camera without it gets the nearest it
offers, 1280x720 at 30 Hz when nothing was chosen. Switching restarts only the
camera and flushes the presenter: the renderer takes each JPEG at its own size,
so the stream stays open. The panel's Aspect Ratio (also a setting) makes
`video_presenter_set_stretch()` scale each axis to the area on its own; each
axis is still in 1/16 steps, so it fills a 4:3 source but leaves a 16:9 one
where it was. `video_presenter_begin()` turns it off, so the players never
stretch, and only `MjpegRenderer` reads the separate vertical scale.

Frames land in four 1 MB slots carved out of the media arena, which is idle
because the player is closed whenever Home is on screen. Four is what can be out at once: one filling, one
waiting to be received, one in the presenter's queue and one being decoded. A
frame larger than a slot is dropped.

The host stack takes alternates with up to three transactions per microframe,
so a capture dongle that asks for 2048-byte payloads gets its 2 x 1024-byte
alternate rather than a single-transaction one that busy pictures may not fit.

The camera's receive buffers are internal RAM (`libs/usb_host` README, Video):
in PSRAM a quarter of an isochronous camera's packets were lost while the
picture was decoded and shown, which reached the decoder as broken JPEGs, and
a bulk camera's buffers there cost its microphone one packet in twelve. A bulk
camera with 16 KB payloads takes 64 KB of internal RAM while it streams.

If the device also records audio, `CapturePlayback` plays it through
`audio_output` with the Video content type, so the volume slider and the
Settings panel's Sound section are the board's. The device clock and the I2S
clock drift apart, which shows up as the capture ring's fill: it is prefilled
to 20 ms, and while its smoothed fill is more than 3 ms off, each 5 ms chunk
drops or repeats one frame (at most 0.4 %). A read that times out prefills
again.

On the simulator, `run.sh` points `SIMULATOR_USBH_UVC_PATH` at `simulator/uvc`
(gitignored, any 1280x720 JPEGs); see `simulator/verify/video_input.txt` and,
for the Input Format panel, `video_input_format.txt`.

## Wi-Fi

The radio is the C6 coprocessor behind esp-devkit's `libs/wifi` and
`libs/esp_hosted_enhanced`. The SDIO link to it is brought up by
`esp_wifi_init()`, i.e. when Wi-Fi is switched on, not at boot: with Wi-Fi off
the board spends no internal SRAM on it. Switched on, the link's DMA
descriptors and the `hosted_tx`, `hosted_rx` and `wifi_mgr` task stacks come out
of internal SRAM, which is the same pool the video decoders' PIE task stacks
need (see [Shared SRAM buffer](#shared-sram-buffer)).

`wifi::Manager` keeps the credentials in its own `wifi` NVS namespace but not
whether the radio is on, so that is a setting here (`settings_wifi_enabled`).
`settings_apply()` calls `set_enabled(true)` for it, which also rejoins the
saved network; the manager is not touched at all while the setting is off.

The SD card and the SDIO link share the SDMMC controller; the BSP routes the
card's commands through `esp_hosted_enhanced`'s hooks. Wi-Fi stays on during
playback, so its traffic and the card reads contend for the controller.

The RX buffers (`CONFIG_WIFI_RMT_*`) are sized for steady streaming receive
rather than for the least memory.

`WifiPage` registers itself as the manager's listener, which is a single slot;
anything else that wants Wi-Fi state while the page exists has to share it.

On the simulator `libs/wifi` runs its fake backend, driven by the `wifi-aps`,
`wifi-connect-result`, `wifi-delay` and `wifi-drop` harness commands.
`simulator/verify/wifi.txt` expects Wi-Fi off and switches it off again at the
end, because the setting persists in `simulator/nvs_data.json`.

## Harness

`CONFIG_HARNESS=y` is set for the device build (the simulator always has it),
so `./run.sh simulator --verify` / `./run.sh esp32p4 --verify` can inject touches and read the panel
back as JPEG. The console is USB-Serial-JTAG, which is what keeps full-panel
captures fast. The harness links the JPEG encoder, which is part of why the factory
partition is larger than `singleapp`'s default; the Japanese glyph packs
([`fonts.md`](fonts.md)) are the rest, and took it from 4M to 6M. Coordinates in scripts are
panel pixels (720x1280 portrait) whatever the UI rotation. Scripts that need
landscape inject it with `imu rot90`; the headless simulator otherwise stays at
rotation 0. See `esp-devkit/docs/harness.md`.

Opening the USB-Serial-JTAG port resets the board, so every
`./run.sh esp32p4 --verify` starts from a fresh boot on the Home screen. A
script cannot pick up where a previous run left off; drive the whole path from
Home in one script.
