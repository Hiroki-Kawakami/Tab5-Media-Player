# Playback

What plays today: MJPEG `*.avi` with PCM or MP3 audio (or none), full screen,
with play/pause, restart, seek, loop and volume. Picking a `.avi` in the file
browser opens `PlayerScreen`.

The planned additions are MKV, H.264 at low resolutions, audio-only files,
images, playlists, playing a directory in order, and a path that decodes a
720x1280 MJPEG frame straight into the framebuffer. None of them exist yet.
Their names are in the layout so you can see where each would land. Only the
decisions that are expensive to reverse later were taken now.

## Layers

```
PlayerScreen        LVGL overlay, transport UI
  │ player_open(path) / play / seek(us) ...
Player              command task, state machine, media clock, pacing, loop
  │
Demuxer ──Packet──▶ ring ──▶ video_presenter ─┬─ MjpegDecoder ──▶ scratch (leased)
  (AviDemuxer)        │                        └─ PPA rotate/scale ─▶ FB ─▶ compose overlay ─▶ present
                      └──▶ audio_out (PCM / MP3) ──▶ bsp_audio_write
```

| path | role |
|---|---|
| `components/avi_demux/` | RIFF/AVI parsing and aligned read-ahead, plain C with no app types |
| `app/media/` | `Demuxer` interface, `MediaInfo`/`Packet`, the AVI adapter |
| `app/playback/` | `Player`: reader, audio and pacing tasks |
| `app/video/` | `MjpegDecoder`, `video_presenter` (framebuffers, PPA, overlay) |
| `app/audio/` | `audio_out`: BSP output, MP3 decode, playback position |
| `app/screens/player_screen.*` | the full-screen player UI |

## Decisions taken now because they are expensive later

**The timeline is microseconds, not frame numbers.** `Packet::pts_us`,
`Player`'s due times, seek targets and `PlayerStatus` are all in µs. AVI has no
timestamps, so `AviDemuxer` derives them as `frame_index × frame_interval_us`.
MKV carries real timestamps, variable frame rate exists, and audio-only files
have no frames. A frame-index timeline would have to be replaced everywhere at
once.

**Containers and codecs are separate.** A `Demuxer` produces bytes tagged with
a `CodecId`. It knows nothing about JPEG, and nothing above it knows about
RIFF. `Packet::keyframe` already exists (always true for MJPEG) because H.264
seeking and frame dropping will need it. `demuxer_create()` dispatches on the
file extension. Probing magic bytes can replace that inside the same function.

**The decoder asks the presenter for its output buffer.**
`MjpegDecoder::decode()` takes a `FrameAllocator` and calls `lease(pic_w, pic_h)`
after it has read the JPEG header. Today the only allocator is the presenter's
scratch buffer, followed by a PPA pass into the framebuffer. The planned direct
path fits this without touching the decoder: when the frame is exactly 720x1280
after MCU padding and the rotation is 0, `lease` returns the off-screen
framebuffer and the PPA step is skipped. The decoder already outputs the
panel's pixel format (see below), so this holds for RGB565 and RGB888 alike. Before building it, check
two things:

- **Framebuffer alignment.** `jpeg_enh_decoder_process` needs a
  cache-line-aligned buffer. Confirm the DPI panel's framebuffers are aligned.
- **Repaint.** The current repaint re-blits the scratch buffer. A framebuffer
  already has the overlay composed into it, so copying one would bring the bar
  back. The direct path would have to re-decode a kept copy of the last
  compressed frame instead.

**The pixel format follows the panel.** `video_presenter_begin()` reads
`bsp_display_get_pixel_format()`. That one value decides the JPEG decoder's
output format, the scratch buffer's bytes per pixel, the framebuffer size, and
the PPA SRM input and output color modes. Nothing in `app/video/` assumes
RGB565, so switching `bsp_config.display.pixel_format` to RGB888 in
`app/media_player.cpp` is enough on the video side. `MjpegDecoder::open()`
recreates the decoder if the format differs from the one it was created with.

- **Byte order.** Tab5 and simulator RGB888 framebuffers hold bytes as B, G, R
  (LVGL's native order). The decoder runs with `JPEG_DEC_RGB_ELEMENT_ORDER_BGR`
  for both formats, which matches, so PPA needs no `rgb_swap`.
- **Unsupported formats.** Any other format (L8) makes `begin` fail, and the
  player screen shows "unsupported panel pixel format for video".
- **Memory.** In RGB888, each of the three framebuffers grows from 1.8 MB to
  2.8 MB, and so does the scratch buffer for a 720p frame.
- **Tested.** `simulator/verify/player.txt` passes with the panel switched to
  RGB888, with correct colors and repaint. The device has not been run in
  RGB888.

**The player only opens paths.** It has no idea what comes next. Playing a
directory in order or a playlist becomes a layer that watches for `Finished`
and calls `player_open()` again. The screen is called `PlayerScreen`, not
`VideoScreen`, because audio files will use it too.

## Deliberately not done yet

- **No abstract video decoder.** MJPEG is the only codec, so `MjpegDecoder` is
  a concrete class used directly by `video_presenter`. Introduce the interface
  with the second codec.
- **Dropping late frames stays in the player, before decode.** That is correct
  only because every MJPEG frame stands alone. With H.264 a skipped frame
  breaks every later frame that references it. Late frames will need to be
  decoded but not presented, or skipped until the next keyframe. The drop
  branch in `step_playing()` is the code to change.
- **`buffered_reader` still lives inside `avi_demux`.** Move it into a shared
  component when a second demuxer needs it.
- **Rotation is fixed at `BSP_ROTATION_90`.** Landscape clips fill the panel,
  and a portrait clip is scaled down into the rotated frame. Following the
  video's orientation also means rebuilding the overlay, which is laid out for
  landscape.

## Reading: staying on FatFs's fast path

FatFs only reads straight into the caller's buffer, via DMA, for whole sectors.
A partial sector at either end of a read goes through its window buffer and
costs an extra copy. AVI chunks sit at whatever offset the muxer chose, so
`buffered_reader` reads 64 KB chunks at sector-aligned offsets on its own task
(`avi_reader`) and copies arbitrary ranges out of them. It uses `open()/read()`
rather than `fopen/fread`. The file I/O happens outside the reader's state
lock, so a cache miss does not block the other side for a whole read.

Three things about AVIs from other tools:

- **Streams are matched by `strl` order, not by fixed chunk ids.** A file that
  declares audio first still plays.
- **`idx1` offsets are ambiguous.** Some muxers make them relative to `movi`,
  others make them absolute. The demuxer tries both against the first video
  chunk. If neither matches, seeking is disabled and playback still works.
- **`suggested_buffer_size` is ignored.** Muxers often write 0 there. Ring
  slots are sized from the largest chunk listed in `idx1`, and a chunk that
  does not fit its slot is skipped.

An AVI written by ffmpeg (`-c:v mjpeg -pix_fmt yuvj420p`, with PCM or
`libmp3lame` audio) plays on the simulator. The P4's hardware decoder only
accepts baseline JPEG, and `jpeg_image_size()` rejects anything else with a
message.

## Pacing and the clock

- **Due times are absolute.** A frame is due at
  `origin + (pts - origin_pts)`, so one slow frame does not delay the rest. A
  frame more than one interval late is dropped instead of shown, except the
  last frame, which is always shown.
- **The clock is wall time, corrected only downwards by audio.** `bsp_audio_write`
  blocks when the device buffer is full, so the written position runs ahead of
  what is audible by one buffer. Slaving video to it would show a burst of
  frames at every Play and keep the picture ahead of the sound. The clock is
  pulled back only when audio falls more than 250 ms behind, which happens when
  the card stalls.
- **Waits are FreeRTOS ticks** (`CONFIG_FREERTOS_HZ=100`, so 10 ms). That is the
  knob if pacing jitter ever matters.

## Tasks and teardown

| task | prio | work |
|---|---|---|
| `media_reader` | 4 | `Demuxer::read` → video ring (4 slots) / audio ring (8 slots) |
| `avi_reader` | 3 | 64 KB aligned read-ahead |
| `player` | 5 | commands, pacing, submit to the presenter |
| `video_presenter` | 5 | decode → PPA → compose overlay → present |
| `media_audio` | 6 | audio ring → `audio_out_write` |

Audio has the highest priority because a late audio write is audible and a
late frame is not.

Every session change goes through `reader_stop()`, which is the only safe way
to reuse ring slots:

1. Park the audio task.
2. Park the reader.
3. Flush the presenter, waiting for the frame being drawn.
4. Refill the free queues.

- **Parking uses "idle tokens".** Each task gives its token back when it
  parks, so "token available" always means "task is parked", no matter how
  many stops have happened.
- **Flushing must wait for the frame in flight.** Emptying the queue is not
  enough. A frame still being decoded would release its slot after the reset,
  putting that slot in the free queue twice.
- **Never wait for another task with a short `vTaskDelay`.** With a 100 Hz tick,
  `pdMS_TO_TICKS(n)` for `n < 10` is 0, which only yields, and a yield never
  hands the CPU to a lower-priority task. The player (prio 5) waiting on the
  reader (prio 4) that way spins until the task watchdog fires on `IDLE0`. Use
  a semaphore.
- **Slots are allocated on the first open, grown when needed, and never
  freed.** That is what makes `close()` safe while the presenter may still be
  reading a slot.

## Looping

Looping is seamless. When `read()` reaches the end, the **reader** seeks back
to 0 and keeps filling the ring, so the picture never drains and audio never
closes. Restarting through `reader_stop()` would cause a visible hitch at every
wrap.

- **The player detects the wrap because pts goes backwards.** It then advances
  the origin by exactly one pass (`next_pts - origin_pts`), not to "now".
  Resetting to now would give the last frame zero duration, and the loop point
  would drift.
- **Turning loop on after the reader has already stopped at the end re-arms
  it.** `handle_loop` seeks to 0 and wakes the reader, but only if it can take
  the reader's idle token, which proves the reader is parked.
- **With loop off, a frame whose pts is earlier than the play origin is
  discarded.** This covers a seek that lands before its target.
- **Each wrap throws away the read-ahead cache.** The backward seek invalidates
  it.

## The overlay

`video_presenter` owns the framebuffers while the player is open. The main
LVGL display is hidden. The controls are a separate 160 px LVGL display
(`DisplayPresentMode::Deferred`, rotated 90°) that the presenter composes over
each picture before presenting.

- **The video sets the update rate.** LVGL only marks the overlay dirty on
  `LV_EVENT_RENDER_READY`, and the next frame picks it up. While paused, the
  presenter wakes every 100 ms and presents a dirty overlay on its own.
  Otherwise the Play button would never change.
- **`compose()` cannot erase.** Hiding the bar would leave its pixels on screen
  with nothing to paint over them. `video_presenter_repaint()` re-blits the
  last decoded frame from the scratch buffer. That costs one PPA operation, no
  decode.
- **`fb_num` is 3.** With two framebuffers, the next frame's output would compete
  with the one being scanned out.
- **Decoding uses the whole-frame API (`jpeg_enh_decoder_process`), not the
  strip pipeline.** The pipeline's final wait is `portMAX_DELAY`. One lost
  strip event would freeze the task that owns the panel.

## Audio

- **PCM is copied before it goes out.** `bsp_audio_write` runs the BSP's DSP
  chain in place on the buffer it is given. `audio_out` copies PCM into its own
  buffer first, so a ring slot is never passed directly. Decoded MP3 already
  lands in that buffer.
- **The playback position is PCM frames written divided by the sample rate.**
  It feeds the clock correction above.
- **The device MP3 decoder is `espressif/esp_audio_codec`, pinned below 2.6.**
  2.6 and later refuse to build unless the ESP32-P4 is chip revision 3.0 or
  newer, and this board's P4 is older (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`).
  Check the silicon before raising the pin. The dependency is device-only.
- **The simulator decodes MP3 with ffmpeg's libavcodec.** It is linked in
  `simulator/CMakeLists.txt`, and `flake.nix` provides it.

## Simulator

`simulator/verify/player.txt` goes Home → SD Card → `Movies/`, then exercises
poster, play, pause, hiding and showing the bar, seek, restart, loop and back
on `Movies/clip.avi`. It then plays `Movies/ffmpeg.avi`. Both files are local
fixtures in the gitignored `simulator/sdcard`. To make an ffmpeg fixture:

```sh
nix develop -c ffmpeg -f lavfi -i testsrc=size=1280x720:rate=15 \
    -f lavfi -i sine=frequency=440:sample_rate=44100 -t 4 \
    -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a libmp3lame -b:a 128k \
    simulator/sdcard/Movies/ffmpeg.avi
```

- **Seek drags need a `wait` between `down` and each `move`.** A `move` in the
  same frame as the `down` never becomes a drag, and the seek silently does
  nothing.
- **`[mp3float] overread` after a seek is harmless.** ffmpeg's MP3 parser was
  handed a chunk from the middle of a frame and resynchronises.
- **Do not measure frame rate here.** The host decodes JPEG in software, so a
  10 fps clip shows about 8-9 fps.
