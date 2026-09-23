# Playback

What plays today: MJPEG, H.264 or progressive MPEG-2 (up to 1280x720) in
`*.avi`, `*.mkv` and `*.mp4`/`*.m4v`/`*.mov` with PCM, MP3, IMA ADPCM or AAC
audio, plus Opus in MKV and MP4 (or no audio), full screen, with play/pause, restart, seek, loop and
volume. Picking one in the file browser opens `VideoPlayerScreen`. The H.264 and
MPEG-2 decoders themselves are described in [`h264.md`](h264.md) and
[`mpeg2.md`](mpeg2.md).

Audio-only files play too, in `AudioPlayerScreen` (see
[Audio-only files](#audio-only-files)): `*.m4a` (AAC, MP3, Opus or PCM in MP4),
`*.wav` (16/24/32-bit PCM or IMA ADPCM), and `*.mp3` / `*.aac` as elementary
streams. `.opus`/Ogg is deliberately left out for now.

Both screens play the files of one directory in order (see
[Playlists](#playlists)). The planned additions are images and stored playlist
files; neither exists yet. Their names are in the layout so you can see where
each would land. Only the decisions that are expensive to reverse later were
taken now.

## Layers

```
VideoPlayerScreen   LVGL bar (Partial, straight into FB 0), transport UI
  │ player_open(path) / play / seek(us) ...
Player              command task, state machine, media clock, loop
  │                 video_pacing: submit, drop, keyframe resync
  │
Demuxer ──Packet──▶ ring ──▶ video_presenter ──▶ MjpegRenderer ─┬─ JPEG ▶ FB            (direct)
  (Avi/Mkv/Mp4)       │         placement, FB,  │                  └─ JPEG ▶ strips ▶ PPA ▶ FB (pipeline)
  │                   │         letterbox,      └─ decoder task ─▶ H264Renderer / Mpeg2Renderer ▶ packed DPB
  │                   │         UI clip ◀── ready queue ◀──────────────┘   ▶ PPA (YUV420) ▶ FB
media_buffer          │           └─▶ present
  (arena)             └──▶ audio_out (PCM / MP3 / ADPCM / AAC / Opus) ──▶ bsp_audio_write
```

| path | role |
|---|---|
| `components/media_buffer/` | aligned read-ahead into the arena, packets as views into it |
| `components/riff_demux/` | RIFF parsing, plain C with no app types: the chunk walk and WAVEFORMATEX in `riff.c`, then `avi_demux` and `wav_demux` on top |
| `components/es_audio_demux/` | MP3 and ADTS AAC elementary streams, plain C with no app types |
| `components/mkv_demux/` | EBML/Matroska parsing, plain C with no app types |
| `components/mp4_demux/` | ISO BMFF/QuickTime parsing, plain C with no app types |
| `components/vdec_common/` | PIE kernels on the packed picture layout and the thread hooks, shared by both decoders |
| `components/h264_dec/` | the H.264 decoder, plain C, host-testable (see [`h264.md`](h264.md)) |
| `components/mpeg2_dec/` | the MPEG-2 decoder, plain C, host-testable (see [`mpeg2.md`](mpeg2.md)) |
| `app/media/` | `Demuxer` interface, `MediaInfo`/`Packet`, the AVI, WAV, MKV, MP4 and elementary-stream adapters |
| `app/playback/` | `player.cpp`: commands, state machine, reader, audio and the media clock; `video_pacing.cpp`: everything that only exists because there is a picture; `playlist.cpp`: the item list and the cursor both screens step through |
| `app/video/` | `video_presenter` (placement, framebuffers, UI clip, decode/present stages), `VideoRenderer` and its MJPEG, H.264 and MPEG-2 implementations (the latter two share `PackedYuvScaler` for the PPA call), the FreeRTOS hooks the decoders run on |
| `app/audio/` | `audio_out`: BSP output, compressed audio decode, playback position; `ima_adpcm` |
| `app/screens/video_player_screen.*` | the full-screen player UI |
| `app/screens/audio_player_screen.*` | the audio-only player UI, on the main display |
| `app/screens/media_controls.*` | the transport widgets both screens build (icon button, slider, time text, the volume row's behaviour) |

## Decisions taken now because they are expensive later

**The timeline is microseconds, not frame numbers.** `Packet::pts_us`,
`Player`'s due times, seek targets and `PlayerStatus` are all in µs. AVI has no
timestamps, so `AviDemuxer` derives them as `frame_index × frame_interval_us`.
MKV carries real timestamps, variable frame rate exists, and audio-only files
have no frames. A frame-index timeline would have to be replaced everywhere at
once.

**Containers and codecs are separate.** A `Demuxer` produces bytes tagged with
a `CodecId`. It knows nothing about JPEG, and nothing above it knows about
RIFF. `Packet::keyframe` comes from the container (always true for MJPEG).
`demuxer_create()` dispatches on the file extension. Probing magic bytes can
replace that inside the same function.

- **H.264 parameter sets are always Annex-B.** For `CodecId::H264`,
  `TrackInfo::codec_private` holds `00 00 00 01 SPS … 00 00 00 01 PPS …`
  whatever the container stored (avcC in MKV and MP4; avcC, Annex-B or
  nothing in AVI), converted by `h264_config_to_annexb()` in `demuxer.cpp`, so the
  decoder never parses avcC. The packets themselves are not rewritten:
  `TrackInfo::nal_length_size` says how they are framed (0 = start codes,
  otherwise the avcC length size). An empty `codec_private` means the SPS/PPS
  are only in-band.

**The presenter places, the renderer draws.** `video_presenter` owns
everything that does not depend on the codec: the fit (output rotation, a scale of
`n/16`, the output rect), which framebuffer is next, clearing the letterbox,
the area the UI leaves to the video and presenting. A `VideoRenderer` turns a
packet into a `VideoFrame` (`decode`) and puts a frame into a `RenderTarget`
(framebuffer, rotation, `n`, rect, clip, source size) with `draw`, writing
nothing outside the clip. `draw(nullptr, …)` redraws the frame the renderer
kept, which only the ones holding a decoded picture can do (`needs_source()`).
MJPEG's `decode` only reads the header and the JPEG is decoded
inside `draw`; H.264's `decode` does the real work and `draw` is one PPA call.

- **The scale is `n/16` because PPA quantizes to 1/16** (8-bit integer and
  4-bit fraction). `n` is `floor(16 × min(fit/source))`, so clips are scaled up
  as well as down. The rect is `source × n / 16` rounded down, which is the size
  the driver produces, so centering matches what lands in the framebuffer.
  Strips are 16 rows, so every strip boundary scales to a whole row and the
  pipeline's check never rejects this scale. The simulator's PPA shim
  truncates the same way, so a clipped draw (see [The overlay](#the-overlay))
  cannot spill a pixel into the bar there either.
- **The output rotation is the UI rotation plus the source rotation**, both
  counter-clockwise quarter turns (see [Source rotation](#source-rotation)).
  `place()` fits the undecoded source with that sum, so PPA rotates once.
- **The renderer chooses the path.** When the output rotation is 0, `n == 16`,
  the whole panel, a source equal to the panel with both sides multiples of 16,
  and a framebuffer whose address and size are on 64-byte boundaries, the JPEG
  is decoded straight into the framebuffer with `jpeg_enh_decoder_process()`
  and PPA is skipped. That is a 720x1280 clip on a portrait UI, or a 720x1280
  MKV marked as rotated, held in the matching landscape. Anything else
  goes through `jpeg_ppa_pipeline`. The framebuffer check runs on every frame
  because the DPI driver's allocation (`MALLOC_CAP_DMA` on PSRAM) is not
  documented as aligned.
- **The direct path uses the pipeline's own decoder**
  (`jpeg_ppa_pipeline_get_decoder()`), so there is one JPEG engine. The pipeline
  ignores its frame-start callback outside its own `process()`, which is what
  makes the whole-frame call on that handle safe.
- **The renderer owns each packet it is given** and releases it exactly once.
  MJPEG holds the last drawn packet until the next one is drawn, or until
  `discard()` / `close()`, so a redraw decodes it again. The framebuffer
  cannot be copied back because the bar may already cover part of it. One
  of the four ring slots is therefore always held. A failed draw releases the
  new packet and keeps the old one. H.264 cannot work this way, because a
  packet cannot be decoded twice once later frames have used its picture: it
  releases the packet right after decoding and holds the decoded picture (a
  hold count on a DPB frame) instead.
- **The size limits are per codec.** MJPEG rejects widths over 2560 (16 rows
  of RGB888 must fit one shared SRAM half, see
  [`architecture.md`](architecture.md)) and frames over 1920×1088 pixels.
  H.264 rejects more than 3600 macroblocks or a side over 1280, because its row
  buffers live in the same 240 KiB (see [`h264.md`](h264.md)). Separately, no
  packet can exceed the arena's bounce area (1 MB); larger ones are skipped.

**The pixel format follows the panel.** `video_presenter_begin()` reads
`bsp_display_get_pixel_format()`. That one value decides the strip format
(the JPEG output and the PPA input), the PPA output, and the framebuffer size.
The panel runs in RGB888 (`bsp_config.display.pixel_format` in
`app/media_player.cpp`). Nothing in `app/video/` assumes either format, so
switching back to RGB565 is a one-line change on the video side. The renderer is created per player session, so it always
matches.

- **Byte order.** Tab5 and simulator RGB888 framebuffers hold bytes as B, G, R
  (LVGL's native order). The decoder runs with `JPEG_DEC_RGB_ELEMENT_ORDER_BGR`
  for both formats, which matches, so PPA needs no `rgb_swap`.
- **Unsupported formats.** Any other format (L8) makes `begin` fail, and the
  player screen shows "unsupported panel pixel format for video".
- **Memory.** Each of the three framebuffers is 2.8 MB in RGB888 (1.8 MB in
  RGB565). The strips use the fixed shared SRAM either way, and 2560 px is the
  RGB888 limit.
- **Tested.** All `simulator/verify` scripts pass in RGB888 with correct colors,
  on both the direct and the pipeline path. RGB565 was last checked before the
  renderer split.

**The player only opens paths.** It has no idea what comes next. What plays
after the current file is decided above it, by the screen (see
[Playlists](#playlists)), from a `Finished` state and another `player_open()`.

**One engine, a screen per medium.** `Player` is meant to serve audio-only
files as well, so what it does for any file — the reader, the slot lifetime, the
`media_buffer` pin rules, the state machine, seek and loop — stays in
`player.cpp`, and a second copy of those rules is the mistake this split exists
to prevent. Everything that is there only because there is a picture lives in
`video_pacing.cpp` behind `video_pacing_*`, and a file with no video track
simply never enters it. The two talk through `PlayerCore` in
`player_internal.hpp`: the timeline both of them move, plus the demuxer, the
state and the lock.
The screens do not share. An audio screen keeps the main LVGL display, so it
needs none of `VideoPlayerScreen`'s own display, insets and SRAM handover.

## H.264 playback

The decoder's internals are in [`h264.md`](h264.md). This is how the player and
the presenter drive it.

- **Decode and present are separate stages.** `video_presenter` runs a
  `video_decoder` task next to its worker. H.264 packets go to the decoder,
  decoded frames wait in a two-entry ready queue with an absolute due time, and
  the worker draws each one when it is due. A PPA call on a 640x360 picture
  scaled to the panel takes 13-24 ms and mostly waits for the hardware, so the
  next frame decodes meanwhile; decoding and drawing in one task capped a clip
  that decodes at 38 fps to 19 fps on screen. The worker starts a draw early by
  a running average of how long draws take, and when several frames are due it
  draws only the newest. MJPEG stays single-stage: its decode and scale are one
  hardware pipeline.
- **The player submits H.264 frames early.** `video_pacing_step()` hands a frame
  over 120 ms before it is due (`kDecodeLeadUs`) together with its due time in
  `esp_timer` terms. The poster and anything submitted while paused carry a due
  time of 0, which means "now". Pausing therefore still shows the few frames
  already in the pipeline.
- **Late frames are still decoded and shown.** A frame more than one interval
  late goes to the decoder with its due time of "now", unless its NAL header
  says nothing references it (`nal_ref_idc == 0`, checked by the reader); those
  are skipped undecoded. When the decoder falls behind the picture plays in slow
  motion instead of freezing. The presenter only draws the newest due frame, so
  showing every late frame does not queue up draws. Decoding late frames without
  drawing them, as before, left one refresh every 200 ms and looked frozen.
- **Resync by jumping to a keyframe that is already due.** Once a frame is
  more than 200 ms late, the player asks `Demuxer::keyframeBefore()` for the
  last indexed keyframe at or before the clock, at most every 100 ms. If that
  keyframe comes after the current frame, everything up to it is dropped and
  decoding resumes there. Its due time has already passed, so nothing freezes.
  Skipping to the *next* keyframe froze the picture until that keyframe's time
  came, which is a few seconds with long GOPs. Where there is a keyframe index
  (MP4 `stss`, MKV cues, AVI `idx1`), the lag is bounded by the gap between
  indexed keyframes. MKV cues and the AVI index may be thinned, so an indexed
  keyframe is not always the nearest one.
- **The H.264 video ring is 64 slots, so audio keeps flowing while video
  lags.** The reader takes packets in file order, so a full video ring also
  stops audio. With 4 slots (about 130 ms), any lag stalled the reader and the
  sound cut out in busy scenes. A slot only points into the read buffer, so
  64 slots cost no packet memory, and in steady state the audio ring (not the
  video ring) keeps the reader in step. MJPEG stays at 4, because 64 of its
  30-45 KB frames would pin most of the 3 MB read ring.
- **A full video ring, or no index, falls back to the next keyframe.** A full
  ring drops everything up to the next keyframe after three intervals of
  lateness, even when that keyframe is in the future and the picture freezes
  until it: keeping audio going comes first. Without an index (MKV without
  cues, or an index that did not fit in memory), the same happens past
  `max(500 ms, 5 intervals)`. Before the full-ring rule, the stalled audio
  pulled the clock down with it and hid the lateness, so a 720p clip that
  decodes at 10 fps played in slow motion.
- **The clock ignores audio while the video ring is full.** Same reason: that
  audio gap is caused by the video, so pulling the clock back to it would only
  make the video look on time.
- **Seeking snaps to the keyframe.** `Demuxer::seek` reports where it landed and
  `rewind_to()` takes that as the new position, so the poster is the keyframe
  itself and the slider jumps back to it. Decoding forward to the exact target
  would cost a full GOP of decode time on every drag.
- **A flush restarts the decoder.** `video_presenter_flush()` drains both queues,
  waits for the frame in flight in each stage, drops the held picture and calls
  `restart()`, which makes the decoder skip everything up to the next I
  picture. `reader_stop()` already flushes on every session change, so a seek
  never mixes references from before and after it. A loop wrap does not flush;
  the stream restarts at an IDR, which resets the decoder by itself.
- **Colours come from the VUI.** The PPA input range follows
  `video_full_range_flag`; the matrix is BT.709 when `matrix_coefficients` says
  so, BT.601 for 5 and 6, and otherwise BT.709 from 720 lines up. PPA's YUV420
  input is Espressif's packed `O_UYY_E_VYY`, which is why the decoder stores
  pictures in that layout.

- **Decoded pictures come out in display order, so the player schedules by two
  clocks.** The decoder holds pictures back until their output order is settled
  (see [`h264.md`](h264.md#output-order)), which means a packet's presentation
  time is no longer the time it must be decoded. `video_pacing_step()` hands a packet
  over `s_reorder_lead_us` before its own presentation time and passes that time
  along as the frame's due time; the decoder returns it with whichever picture
  comes out. The lead is learned from the stream: it is the largest amount by
  which a packet's presentation time falls behind the highest one seen so far,
  capped at 16 frame intervals. Containers do not have to carry decode
  timestamps for this, which matters because MKV does not.
- **The tag is how a due time survives reordering.** `H264Renderer` passes the
  due time as the decoder's per-access-unit tag and reads it back off the
  picture; a frame the player asked not to show carries `INT64_MIN` instead and
  is dropped when it comes out.
- **End of stream needs a drain.** The last pictures of a file are still inside
  the decoder when the reader runs out, so the player calls
  `video_presenter_drain()` (a null job on the decode queue) before it reports
  `Finished`. The poster path drains as well, so a paused picture appears
  without waiting for the reorder window to fill.

## MPEG-2

MPEG-2 goes through exactly the H.264 path above: the decode stage, early
submission with the learned reorder lead, due times as decoder tags, the drain
at the end and the restart on flush. What differs:

- **B pictures are the droppable ones.** `mpeg2_dec_droppable()` reads the
  picture coding type, so a late B picture is skipped undecoded, like a
  `nal_ref_idc == 0` H.264 picture.
- **AVI timestamps come from `temporal_reference`.** AVI stores packets in
  decode order and the adapter otherwise numbers them in that order, which
  would give every anchor the due time of the B picture displayed before it.
  For MPEG-2 the adapter remembers the frame index of the last packet carrying
  a GOP header and uses `(that index + temporal_reference) × interval`. That is
  a permutation of the decode-order numbers for every GOP shape ffmpeg writes
  (IBBP, three B pictures, closed GOPs). After a seek the times fall back to
  decode order until the next GOP header, which is the keyframe the seek landed
  on.
- **Containers.** AVI `mpg2`/`MPG2`/`MPEG`, MKV `V_MPEG2`, MP4 `mp4v` with
  object type 0x60-0x65 and QuickTime `m2v1`. The sequence header in the
  extradata, when there is one, is handed up as `codec_private` unchanged and
  probed on open, so MPEG-1 and 4:2:2 fail before the first picture. Seeking and
  the keyframe index are the same as for H.264.
- **Audio that usually comes with MPEG-2 is not decoded.** MP2 and AC-3 are not
  in `esp_audio_codec`; such a track is ignored and the video plays silent.

## Deliberately not done yet

- **No fragmented MP4.** Files with `mvex`/`moof` are rejected.
- **No interlace, MBAFF, FMO, SP/SI slices, 4:2:2/4:4:4 or 10-bit.** See
  [`h264.md`](h264.md#scope).
- **No interlaced MPEG-2, no MPEG-1, no MPEG-PS/TS (`.mpg`, `.ts`, `.vob`).**
  See [`mpeg2.md`](mpeg2.md#scope).
- **No sample aspect ratio.** Every codec is shown with square pixels.
- **No `.opus`**: Ogg needs page parsing and a bisection over granule
  positions, which is worth its own step.
- **No 8-bit WAV** (it is unsigned, and everything below is signed), no MS
  ADPCM, and no MP3 inside WAV.
- **No stored playlists.** The only source of a `Playlist` is a directory
  listing; `.m3u` and friends are not read, and there is no queue the user can
  edit.

## Reading: one buffer from the card to the decoder

FatFs only reads straight into the caller's buffer, via DMA, for whole sectors.
A partial sector at either end of a read goes through its window buffer and
costs an extra copy. Chunks and blocks sit at whatever offset the muxer chose,
so `media_buffer` reads 64 KB chunks at sector-aligned offsets on its own task
(`media_readahead`) into the arena, and a demuxer hands out **views** of that
memory instead of copying packets out. It uses `open()/read()` rather than
`fopen/fread`, and the file I/O happens outside the buffer's state lock.

- **Why views.** Measured on the Tab5 with the old design, copying a 20-45 KB
  frame out of the read-ahead cost 1-2 ms (PSRAM to PSRAM runs at about
  22 MB/s), against about 15 ms for the JPEG decode. The copy also forced
  per-slot buffers sized for the largest frame, which MKV cannot state up front.
  A 60 fps clip that drops frames went from 34.5 to 39.3 fps after the change.
- **The arena is 4 MB, allocated once at boot and never freed**
  (`app_entry()`), so PSRAM fragmentation after many open/close cycles cannot
  make it unavailable. One quarter is a bounce area; the rest is a ring of
  64 KB chunks filled in ring order, so consecutive chunks are adjacent in
  memory. The HW JPEG decoder only needs its input contiguous, not aligned
  (it syncs the input with `ESP_CACHE_MSYNC_FLAG_UNALIGNED`).
- **A view pins the chunks it covers.** Read-ahead never writes into a pinned
  chunk; it waits. A view that would cross the ring's end is copied into the
  bounce area instead, once per trip around the ring, and pins that.
- **Nothing may hold a view longer than the reader takes to lap the ring.**
  Read-ahead parks on a pinned chunk and `mb_view` waits for read-ahead, so a
  pin the reader catches up with from behind stops both, with nothing in the
  log; only the `interrupt()` and `releaseAll()` of the next seek clear it. The
  player therefore pins the last packet it *consumed*, not the last one it
  drew: a pin that follows the reader cannot be lapped, while a pin on the
  frame last drawn freezes as soon as frames run late enough to be dropped
  instead of drawn, and the reader walks into it.
- **A view can wait on the player** (pins held by the player or the ring), so
  `reader_stop()` interrupts the buffer before waiting for the reader to park.
  An interrupted read rewinds the demuxer to the element it started on.
- **The interrupt is dropped as soon as the reader has parked**, not when the
  reader is started again: `Demuxer::seek()` runs in between, and the MP3/AAC
  resync reads the file to find its frame boundaries. With the flag still up
  `wait_for()` refuses every read, so those seeks failed silently — and only
  where the target was outside the ring, which is why short files (the whole
  file resident) and every index-driven seek looked fine.
- **Pins are dropped wholesale** (`releaseAll()`) in `refill_free_queues()`,
  after every holder has been flushed. A packet still queued in a ring is never
  released one by one.
- **Header parsing runs with read-ahead off** and reads the file directly.
  Only the sequential passes (`idx1`, `Cues`, playback) turn it on.

Things about AVIs from other tools:

- **Streams are matched by `strl` order, not by fixed chunk ids.** A file that
  declares audio first still plays.
- **`idx1` offsets are ambiguous.** Some muxers make them relative to `movi`,
  others make them absolute. The demuxer tries both against the first video
  chunk. If neither matches, seeking is disabled and playback still works.
- **`suggested_buffer_size` is ignored.** Muxers often write 0 there. Ring
  slots are sized from the largest chunk listed in `idx1`, and a chunk that
  does not fit its slot is skipped.
- **H.264 and MPEG-2 keyframes come from `idx1` (`AVIIF_KEYFRAME`).** The
  index keeps only keyframes as seek points, so `avi_demux_seek()` lands on the
  last keyframe at or before the target and reports that frame; the adapter
  derives pts from it. Without `idx1` a packet is a keyframe when its first
  slice NAL is IDR (type 5) or its picture header says I, and only `seek(0)`
  works. MJPEG ignores the flags.
- **Zero-size video chunks are frames.** ffmpeg writes them to keep the frame
  clock (its H.264 AVIs have one right after the first frame), so pts counts
  them, matching ffprobe's dts, and every keyframe there is one frame later
  than in the same encode muxed to MKV.
- **ffmpeg's H.264 AVIs have no extradata** (libx264 only emits it with
  `-flags +global_header`, and then as Annex-B). ffmpeg refuses to mux
  length-prefixed H.264 into AVI, so the avcC branch is untested on real files.
  `strf` extradata is cut to `biSize - 40`; the chunk carries a pad byte.

An AVI written by ffmpeg (`-c:v mjpeg -pix_fmt yuvj420p`, with PCM or
`libmp3lame` audio) plays on the simulator. The P4's hardware decoder only
accepts baseline JPEG, and `jpeg_image_size()` rejects anything else with a
message.

## MKV

`mkv_demux` walks the stream flat: `Cluster` and `BlockGroup` are entered, and
every other element is skipped by its size. Top-level IDs never collide with
cluster children, so an unknown-size `Cluster` ends where the next top-level
element starts without tracking the hierarchy.

- **Tracks.** The first video track must be `V_MJPEG`, `V_MPEG4/ISO/AVC` or
  `V_MPEG2` (their `CodecPrivate`, avcC or a sequence header, is copied and
  handed up raw). Audio is
  `A_PCM/INT/LIT`, `A_MPEG/L3`, `A_AAC*`, `A_OPUS`, or `A_MS/ACM` whose
  WAVEFORMATEX is IMA ADPCM (`0x0011`); anything else is ignored as
  unsupported. `CodecPrivate` is handed up as-is, except for `A_MS/ACM`, where
  the rate, channels and block align come from the WAVEFORMATEX and only its
  extra bytes are passed on. A track with `ContentEncodings` (header stripping,
  compression) is unsupported.
- **Lacing.** A laced audio block is split and each frame is a packet of its
  own, because raw AAC and Opus frames carry no length and a decoder cannot
  find the boundaries in concatenated data. mkvmerge laces audio by default;
  ffmpeg never does. Every frame takes its own `mb_view()` instead of one view
  over the whole block: the pending frames then hold no pin, so
  `releaseAll()` in `reader_stop()` cannot pull memory from under them (a
  failed seek restarts the reader without seeking), and an interrupted view
  rewinds to that frame. A block whose lace sizes do not add up is skipped, and
  so is a laced video block.
- **Keyframes.** A `SimpleBlock` has a keyframe flag; a `Block` has none and is
  a keyframe when its `BlockGroup` has no `ReferenceBlock`. The walk is flat,
  so for a video `Block` the rest of its group is read ahead and the reader
  steps back to the payload. That backward seek can throw away the read-ahead
  window, which is acceptable because muxers use `BlockGroup` for video only
  on odd frames (ffmpeg: a duration that differs from the default).
- **Timing.** `DefaultDuration` gives the frame interval; without it, the first
  two video blocks are measured. `Info/Duration` gives the length; without it,
  the last cue plus one interval.
- **Seeking needs `Cues`**, found through `SeekHead` or before the first
  cluster. Without them only `seek(0)` works (restart and loop). A seek jumps to
  the last cue at or before the target. The index is decimated past 60000
  entries.
  - MJPEG skips blocks earlier than half an interval before the target, since
    any frame can be shown. ffmpeg starts a cluster for every MJPEG frame, so
    the index keeps one entry per cluster.
  - H.264 must start at a keyframe, so nothing near the target is skipped:
    blocks before the cue's own time are dropped (a cluster can hold more than
    one cued keyframe, so cues are not merged per cluster) and so are video
    blocks until the first keyframe, in case a cue points at a cluster that
    does not start with one. The first video packet's pts is where the seek
    landed. ffmpeg cues only video keyframes.
- **Timestamps are rounded to `TimestampScale`** (1 ms from ffmpeg), so a frame
  at `k × interval` can be a fraction of a millisecond early. The player
  compares pts with half an interval of tolerance, otherwise a seek would drop
  its first frame or be taken for a loop wrap.
- **The end is known from the reader, not from the duration.** The reader sets
  an EOF flag when it parks at the end, and the player finishes once the ring
  is empty. A `Duration` a little past the last frame no longer leaves it
  playing forever.

## MP4

`mp4_demux` reads `moov` whole into PSRAM (up to 16 MB) and uses the sample
tables in place. They are never expanded per sample, since a two-hour file has
over half a million samples. Each track has a cursor (sample, chunk, current
`stsc`/`stts`/`ctts` run, next sync sample) that steps in constant time; a seek
rebuilds it by walking the runs, which are few.

- **Finding `moov`.** Top-level boxes are skipped by size, so a `moov` after
  `mdat` (no faststart) works at the cost of one seek on open.
- **Tracks.** The best `vide` and `soun` track wins: a supported codec first,
  then the `tkhd` enabled flag. Video is `avc1`/`avc3` (`avcC`), `jpeg`/`mjpa`,
  `m2v1`, or `mp4v` whose esds says MJPEG (object type 0x6C, which is what ffmpeg
  writes for MJPEG in `.mp4`) or MPEG-2 video (0x60-0x65). Audio is `mp4a` with an AAC or MP3 object type (the esds
  may sit inside a QuickTime `wave`), `.mp3`, `Opus`, or 16-bit `sowt`.
  QuickTime sound descriptions v1/v2 are longer, and their extra header is
  skipped.
- **`dOps` is rewritten as an OpusHead**, because that is what both Opus
  decoders take (big-endian fields become little-endian; the mapping table is
  the same).
- **A `sowt` sample is one PCM frame**, and `stsz` often says 1 byte, so the
  size comes from the channel count and a chunk is handed out in packets of up
  to 32 KB rather than one packet per frame.
- **Timing.** pts is dts + ctts − the first edit's `media_time`, plus any
  leading empty edits. ffmpeg's AAC edit skips 1024 priming samples, so the
  first audio packet has a negative pts. Later edits are ignored with a warning.
  The frame interval is the most common `stts` delta; the length is `mvhd`'s,
  or the video `mdhd`'s without it.
- **Rotation** is `-atan2(b, a)` of the `tkhd` matrix, as in ffmpeg's
  `av_display_rotation_get`, which is the same counter-clockwise angle as the
  MKV roll: an ffmpeg `-display_rotation 90` file shows the same as
  `rotated.mkv` in all four UI rotations.
- **Seeking** uses `stss`. H.264 lands on the last sync sample at or before the
  target and reports its pts; MJPEG starts at the first frame after half an
  interval before the target. Audio starts at its first sample at or after where
  the video landed.

**Packets come out in timestamp order, not file order.** Cameras, phones and
Apple tools interleave in chunks of 0.5-1 s (ffmpeg interleaves much finer).
Read in file order, a second of audio fills the 8-slot audio ring and stalls the
reader while the video for that second is still behind it, or a second of video
fills the 4-slot ring and the audio drains. Growing the rings would change the
ring-full rules the H.264 pacing relies on. So the demuxer returns whichever
track's next sample is earlier and reads backwards in the file when needed.

- **`mb_view_at()` is a view at an offset that leaves the cursor alone.** The
  cursor becomes a floor: the demuxer moves it forward to the lower of the two
  tracks' next offsets, so the window keeps everything between them and a
  backward read is a cache hit.
- **Files that are not interleaved at all still play, slowly.** When the view
  would end more than half the ring past the cursor, `mb_view_at()` moves the
  cursor to the view, which drops the window, and the next read of the other
  track drops it again, so every switch re-reads from the card. Such files are
  rare, so nothing smarter is done; the demuxer logs once when the tracks are
  that far apart. On the Tab5, `j_separate_big.mp4` plays but skips a burst of
  frames about once a second, which is accepted. 720p in a normal MP4 plays as
  fast as the same stream in MKV.

## Audio-only files

`demuxer_create()`'s extension table also says whether a file is video or
audio (`MediaKind`), and the file browser opens `AudioPlayerScreen` for the
audio ones. Only `.m4a` is there today; the container is `mp4_demux`, which now
accepts a file with no video track. A file with a video extension and no video
track still opens in `VideoPlayerScreen`, which plays the sound over a black
picture and says "no video track" in the bar, rather than the browser second
guessing the extension.

- **The player skips `video_pacing` entirely.** `handle_open()` branches on
  `info.video.codec == None`: no presenter, no video ring, no poster. The
  frame interval is 0, which is what the seek code checks before snapping a
  target to the frame grid, and a duration of 0 only disables seeking instead
  of failing the open ("video has no timeline" is a video rule).
- **The position comes from the clock, not from packets.** With no frames to
  show, nothing would ever move `shown_us`, so `audio_only_step()` sets it from
  `player_media_clock_us()` on every pass. That clock is still wall time pulled
  back by the audio position, so a card stall drags the slider back with it.
- **The end is "the reader is at EOF and every audio slot is free".** The BSP
  has no drain or queued-bytes call, so the last moment the player can observe
  is the one where `audio_out_write()` has returned for the last packet. The
  device buffer still plays out after that; `Finished` only moves the UI, and
  `bsp_audio_close()` does not happen until the screen leaves. Waiting for the
  clock to reach the duration instead would hang whenever the duration in the
  file is optimistic, because an exhausted audio track drags the clock down.
- **A loop wrap is detected from the clock passing the duration.** The reader
  wraps by itself, and no packet ever reaches the player task, so there is no
  pts going backwards to see (which is how the video path notices). The origin
  is advanced by exactly one pass, as it is there.
  - The reader's "produced something this pass" guard, which stops an empty
    file from seeking to 0 forever, used to count video packets only. An
    audio-only file therefore stopped at the end however loop was set.
- **WAV is the same envelope as AVI, not the same demuxer.** The RIFF chunk
  walk, the `fmt ` WAVEFORMATEX and the format-tag table are literally the same
  bytes, so they live in `riff.c` and both demuxers call them; everything else
  differs (AVI is indexed chunk streams on a frame timeline, WAV is one `data`
  range with no packet boundaries at all). `avi_demux` keeps its frame-numbered
  API; `wav_demux` is in µs like MP4.
  - **Packets are cut by us, not by the file.** A packet is about 32 KB rounded
    down to a whole PCM frame, or to a whole IMA ADPCM block, because
    `audio_out` splits ADPCM by `block_align`. pts and seek are byte arithmetic
    and therefore exact.
  - **A `data` size of 0, or one past the end of the file, means the file is
    what a streaming writer left behind**, so it is clamped to the file size.
  - **`WAVE_FORMAT_EXTENSIBLE` resolves through the first two bytes of the
    subformat GUID**, which is the tag the file would have used without it.
- **MP3 and ADTS AAC share one demuxer.** Both are self-delimiting frames with
  no container: the ID3/APE trimming, the resync and the byte-to-time seek are
  the same code, and only the frame header parser differs. The codec is decided
  at open by trying MP3 and then AAC on the first frames.
  - **A sync word only counts when three frames chain into each other.**
    Compressed payload is full of `FF` bytes, so a single header match finds
    false positives, in the file's first bytes as much as after a seek.
  - **ID3v2 is skipped from the size in its header** (plus the footer when the
    flag is set), and ID3v1 and an APEv2 footer are trimmed off the end, so
    neither is fed to the decoder as audio.
  - **The duration is exact only with Xing/Info or VBRI**, which carry the
    frame count. Without one it is the data size scaled by an average of the
    first 64 frames. One frame is not enough: an encoder's first frames are
    much smaller than its steady state, which sized the 6 s AAC fixture at 7 s.
  - **The Xing frame is not played.** It is a valid, silent frame; it is
    dropped, and the byte-to-time mapping starts after it.
  - **Seeking is an estimate**: the byte position comes from the time ratio
    (through the Xing TOC when there is one), and the time it reports back is
    that byte position converted again, so the slider matches what plays. Only
    CBR lands where it says.
- **MP4 without a video track.** `parse_moov` only insists on one usable
  track; seek moves the audio cursor and reports its own pts, `stss` and the
  keyframe index stay video-only, and the duration falls back to the audio
  `mdhd` when `mvhd` has none. A video track that is present but unsupported
  still fails the open, so a broken video file does not quietly play as audio.

## Tags and cover art

`components/media_tags` holds the struct every demuxer fills, the conversion to
UTF-8 and the ID3v2/ID3v1 parser. ID3 is shared rather than living in
`es_audio_demux` because it turns up in more than one container: a WAV or AVI
`id3 ` chunk is the same bytes.

- **Text is fixed-size UTF-8 inside the struct, not heap strings.** The C info
  structs are plain values the C++ layer copies out of; a pointer per field
  would put an ownership rule on every one of them to save a few hundred bytes.
  The cover art is the only allocation, and `media_tags_free()` at demux close
  is the only rule.
- **The first non-empty value of a field wins.** That is what lets a parser
  walk from the better source to the worse one without tracking which it has
  seen: ID3v2, then the ID3v1 block at the end of the same MP3.
- **The cover bytes are copied once per layer.** The demuxer copies them out of
  a buffer of its own (MP4's `covr` sits inside the `moov` it already holds),
  and the C++ wrapper copies them into a `shared_ptr` the UI can outlive the
  demuxer with — the player task owns the demuxer and closes it without asking
  the screen. 2 MB is the cap; more than that is dropped rather than left to
  compete with the arena for PSRAM.
- **Only JPEG and PNG are kept, decided from the first bytes** rather than the
  declared type, because that is what `image_framework` decodes. Every
  Matroska attachment is offered to the same check, so a font attachment is
  rejected without a rule about names.
- **Matroska `Tags` and `Attachments` may sit after the clusters**, where the
  header walk stops. Their `SeekHead` entries are recorded like the Cues' one
  and read before the walk returns; a file with neither loses nothing.
- **Matroska target levels are ignored.** ffmpeg writes a file-level TITLE at
  the album level (50), so honouring the level would turn every song title into
  an album name.
- **RIFF `LIST INFO` is why the WAV walk no longer stops at `data`.** The list
  is written after the audio as often as before it, and stepping over a chunk
  is a seek.
- **The artwork is decoded on the metadata worker**, never on the LVGL task,
  and the screens ask `media_cache` for pixels that are already there (see
  [`metadata.md`](metadata.md)). Decoding where it is shown was slow enough to
  be felt: a 500 KB cover is 1.2 s of software JPEG on the board (16 ms on the
  host), and the UI — the seek bar included — was frozen for that long as the
  screen opened.
- **Non-ASCII tags render as missing-glyph boxes**, the same limitation file
  names have (see [architecture](architecture.md#sd-card)).

## The audio screen

`AudioPlayerScreen` is a plain `ScreenManager` screen on the main display, so
none of `VideoPlayerScreen`'s machinery applies: no display of its own, no SRAM
handover, no insets, no rotation listener, and no auto-hide, because the UI is
the whole screen rather than something sitting on top of a picture. Rotation is
`LV_EVENT_SIZE_CHANGED` on the root, which rebuilds the contents on the next
LVGL tick like Home does.

- **The artwork is one fixed 552 px square in both orientations**, so that a
  rotation can reuse the artwork it has already decoded rather than asking for
  another size. The number is what the landscape layout happened to leave and
  is meant to be tuned by hand, not derived. It is still built after the
  controls and, in portrait, moved in front of them.
- **The title is the tag title and the second line is artist and album**,
  falling back to the file name and the audio format when the file carries no
  tags; a failure or an audio note still takes the second line, in orange. The
  navigation bar keeps the file name either way, so the file a title belongs to
  stays identifiable.
- **The title is resolved before the screen appears.** It comes from
  `media_cache` (the browser has usually prefetched the row), not from the
  player's summary, which is only valid once the file is open — that was what
  showed the file name first and swapped in the title a moment later. The
  artwork still arrives afterwards and replaces the note icon in place rather
  than rebuilding the layout; a picture that fills in changes no text.
- **The next and previous tracks are prefetched** two seconds into playback, so
  a skip has its title and artwork ready. The requests are cancelled when the
  track changes.
- **The transport widgets are shared with the video player**
  (`app/screens/media_controls.*`): the icon buttons, the sliders, the time
  text and the whole behaviour of the volume row, which is the part that must
  not drift between the two screens. Only the colours differ, and they are
  arguments.

## Source rotation

An MKV track's `Video/Projection/ProjectionPoseRoll` is honoured when
`ProjectionType` is 0 (or absent) and yaw and pitch are 0. It is rounded to a
quarter turn; a roll that is not a multiple of 90 is ignored with a warning.

- **Roll is counter-clockwise, and so are `bsp_rotation_t` and PPA's angle**,
  so +90 maps to `BSP_ROTATION_90`. ffmpeg writes +90 for
  `-display_rotation 90` and reads it back as `rotation=90`.
  `simulator/verify/mkv.txt` shows the picture upright in all four UI
  rotations.
- **The point is the direct path.** A 1280x720 picture stored as 720x1280
  (`transpose=clock`) with roll +90 decodes straight into the framebuffer when
  the UI is at 270. At 90 the output rotation is 180 and it goes through PPA;
  store it with roll -90 (`transpose=cclock`) if the other landscape is the one
  in use.
- **The player sets it on open** (`video_presenter_set_source_rotation`). The
  presenter applies it between frames like a UI rotation.

## Pacing and the clock

- **Due times are absolute.** A frame is due at
  `origin + (pts - origin_pts)`, so one slow frame does not delay the rest. A
  frame more than one interval late is handled by the late-frame rules in
  [H.264 playback](#h264-playback); for MJPEG it is dropped, except the last
  frame, which is always shown.
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
| `media_readahead` | 3 | 64 KB aligned read-ahead into the arena |
| `player` | 5 | commands, pacing, submit to the presenter |
| `video_presenter` | 6 | (MJPEG: decode →) PPA → present, core 0 |
| `video_decoder` | 2 | H.264 decode (parse, prediction, residual) or half of the MPEG-2 rows, core 0 |
| `h264_post` | 2 | H.264 deblocking, packing, frame writes, reference window, core 1 |
| `mpeg2_rows` | 2 | the other half of the MPEG-2 rows, core 1 |
| `media_audio` | 6 | audio ring → `audio_out_write` |
| `player_notify` | 2 | posts the state callback to the LVGL thread, stack in PSRAM |
| `media_meta` | 2 | tags, cover art and thumbnails, core 0, stack in PSRAM (see [`metadata.md`](metadata.md)); stopped while the video player is open |

Audio has the highest priority because a late audio write is audible and a
late frame is not. The presenter sits at 6 too, on core 0 with the decoder,
so it wakes on time while the decoder is busy; it spends most of its time
waiting for PPA.

The two decoder workers (of either codec) sit below everything else because they are the only
tasks that use a whole core. At 720p they never block, and above the reader
(4), the read-ahead (3) and LVGL (4) they would starve all three. The decoder
also sleeps 20 ms after 500 ms without blocking, which keeps `IDLE0` fed for
the task watchdog. Taking a harness capture while a heavy clip plays still
trips the watchdog, since the capture itself runs on core 0 for seconds.

`media_audio` has a 20 KB stack because the decoders run on it. Opus's CELT
decoder keeps its work buffers in stack VLAs and overflowed the 4 KB that was
enough for MP3; `esp_audio_codec`'s README asks for about 20 KB. The simulator
never shows this, since host threads have large stacks.

Every session change goes through `reader_stop()`, which is the only safe way
to reuse ring slots:

1. Park the audio task.
2. Interrupt the buffer and park the reader.
3. Flush the presenter, waiting for the frame being drawn.
4. Drop every pin and refill the free queues.

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
- **Ring slots hold no memory**, only a view and its pin. The bytes live in the
  arena, which outlives every session, so `close()` is safe while the presenter
  may still be reading a slot.

- **A stop that times out must not continue.** Every wait above (and the ones
  in `media_cache_stop()`, `video_presenter_flush()`, `video_presenter_end()`
  and `mb_close()`) loops until the task really parked, logging each round.
  They used to give up after a second or two and free the buffer, the demuxer
  or the renderer anyway, which is a use-after-free whenever the task was only
  slow — and on USB a single read stalls for seconds when playback is reading
  the same device. A stall is now a stalled UI and a log line instead of a
  corrupted heap that panics minutes later somewhere else.

Ending a session is the same stop plus a wait: `player_close()` does not return
until the player task has run `handle_close()`. The caller frees what the
pipeline is still holding — `VideoPlayerScreen::onExit()` destroys the
presenter and its renderer and hands the shared SRAM back to LVGL — so a
queued-and-forgotten close leaves those tasks running against freed memory.
Closing the screen right in the moment a file switch had just opened the next
one was enough to see it: the LVGL thread's own `video_presenter_flush()`
released jobs, and `Demuxer::release()` reached a demuxer the player task was
destroying, which left `mb_release()` decrementing pins through a dangling
pointer somewhere in PSRAM.

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

## Playlists

A `Playlist` (`app/playback/playlist.*`) is a list of `{name, path}` plus a
cursor, and nothing else: no filesystem, no `MediaKind`, no player calls. It is
what both screens are constructed with, so a screen has no path of its own —
`name()` and `path()` read the item under the cursor. Stepping it is
`step(delta, repeat)`, which is also the single place the wrap rule lives.

- **The screens' list comes from the file browser, not from a second scan.**
  `FileBrowserPage` already holds the directory listing, and on FAT another
  `opendir` walk over a few hundred files is not free. It hands over the
  entries of the same `MediaKind` as the file that was tapped, in the order it
  shows them, so a music file plays the directory's music and a video its
  videos. A different source (an `.m3u`, a queue) fills the same vector.
- **A caller with no listing reads one with `playlist_items_at()`**, next to
  the class: a directory's files of one kind, or the path itself when it is a
  file of that kind. The slideshow's music is that caller. The listing and its
  order are `media_directory_list()` (`app/media/media_directory.*`), the same
  one the browser shows, so both put the files in the same order.
- **The player is untouched.** Everything here is the screen reacting to
  `PlayerState::Finished` with another `player_open()`, which is what
  [the layering](#decisions-taken-now-because-they-are-expensive-later) already
  assumed. Nothing about a list reaches `player.cpp`.
- **Repeat is off / all / one.** Off stops at the end of the list, all wraps
  around it, and one loops the current file through `player_set_loop()` — the
  only one of the three the player knows about, because looping one file
  seamlessly is something only the reader can do (see [Looping](#looping)).
  The other two therefore never set the player's loop flag.
- **Shuffle covers the whole list, not what follows the current file.** The
  current file stays where playback is and every other file, earlier ones
  included, is drawn after it — the same as turning shuffle on in any music
  player. Turning it off carries on from the current file in the browser's
  order. With repeat all, running off the end draws a new order, and the file
  that just played is kept out of its first slot; running off the start with
  the previous button goes to the end of the current order instead, since the
  order before it is gone.
- **Repeat and shuffle are settings, one pair for music and one for video**,
  so a screen opens with the last choice made in that kind of screen. A screen
  opened with shuffle on starts the tapped file and shuffles the rest.
- **The previous button restarts the file if it is more than 3 s in**, and
  moves to the previous item otherwise, as every music player does. With
  nothing to move to it restarts either way. Restarting keeps playing, where
  `player_restart()` alone leaves the player paused.
- **A file that fails to open is skipped only when it was opened
  automatically**, and at most as many times in a row as the list is long, so a
  directory of broken files stops instead of spinning. Pressing next onto a
  broken file shows the failure, like opening it from the browser does.
- **The screens wait for the new file before showing anything of it.**
  `player_open()` is a command in a queue: until the player picks it up, its
  status and summary still describe the previous file, and a refresh in that
  window would put the old cover art, duration and note under the new name. Any
  such refresh is gated by the `awaiting_start_` flag, which the player's
  `Paused` transition clears — `handle_open()` publishes the summary before it
  sets that state.

## The state callback

`player_observe_state()` takes one function that the player calls when its
state changes. It is what makes the next file start the moment the last one
ends, instead of at the next refresh tick. The screens still poll on their
500 ms timer, and that timer still calls the same handler, so a missed
notification costs latency and nothing else.

- **The callback carries no state.** The handler reads `player_status()` again.
  The notification is delivered late and several changes in a row collapse into
  one, so by the time it runs the player may have moved on — most importantly
  past a `Finished` the screen already reacted to. A handler that trusted a
  state it was handed would advance twice.
- **A task of its own posts it.** `player_set_state()` only gives a semaphore;
  `player_notify` is what takes the LVGL lock and hands the handler to
  `lv_async_call`. None of the tasks a teardown waits for may reach for that
  lock: `player_close()` blocks the LVGL thread while it holds it (see [Tasks
  and teardown](#tasks-and-teardown)), so the player task calling `lv_lock()`
  there deadlocks the two. Keeping the hop in one task also settles the older
  hazard of taking the LVGL lock and `player_core.lock` in opposite orders.
- **Not a periodic LVGL timer.** Polling a change counter from a short-period
  timer needs no extra task, but it wakes the LVGL task tens of times a second
  for nothing, and that was enough to lose the video a repaint: after a
  rotation the overlay's black pass and the next frame race for framebuffer 0,
  and with the timer running the video area stayed black until the following
  frame.

## The overlay

`video_presenter` owns the framebuffers while the player is open. The main
LVGL display is hidden. The controls are two separate LVGL displays
(`DisplayRenderMode::Partial`) along the top and bottom edges as the user sees
them: an 80 px top bar, and a bottom bar of 160 px in landscape and 272 px in
portrait. Icons come from `app/resources` (Tabler, see [`resources.md`](resources.md)).

- **The bars hide themselves after 4 s without a touch while playing.**
  Inactivity is `lv_display_get_inactive_time(nullptr)`, which covers both
  bars; a touch on the video goes through the outside-touch callback instead,
  so it calls `lv_display_trigger_activity` itself. Holding a slider still
  sends no touch events, so a pressed slider blocks hiding explicitly. When
  playback finishes or fails the bars come back on their own, otherwise a clip
  that ended hidden leaves nothing on screen to act on.
- **Time and seek position refresh on a 500 ms timer but redraw at most once a
  second.** LVGL invalidates on every `lv_label_set_text`, so the labels are
  only set when the displayed second changes; `lv_slider_set_value` already
  returns early on an unchanged value. Nothing is refreshed while hidden
  (a hidden display drops invalidations anyway).

- **While the bar is shown, video and LVGL share framebuffer 0 and the video
  tears.** Composing the bar into every frame tied LVGL's rendering to the
  video's buffer swaps: a slider drawn between two swaps came out torn, the
  copy cost frame rate (a 360p clip fell from 30 to 20 fps), and it grew with
  the bar. Now `video_presenter_set_ui_insets()` gives the presenter the edges
  the UI owns; it stops swapping, draws every frame into framebuffer 0 clipped
  to the rest, and LVGL blits only what changed straight into the same
  framebuffer. Hidden, the insets are zero and the three-buffer swap (no
  tearing) is back.
- **Framebuffer 0, because both BSPs agree on it.** The device's partial blits
  go to the framebuffer last flushed, the simulator's always to 0, and a
  partial flush presents index 0 on both.
- **The bars render in RGB565 whatever the panel is.**
  `DisplayManagerConfig::color_format` picks the LVGL format independently of
  the panel; the BSP blit converts on the way in (Partial needs
  `BSP_DISPLAY_CAP_CONVERT`, which the MIPI DSI panel and the simulator have).
  The video is the content that wants the extra bits, not the controls, and
  565 halves both the draw buffers and the bytes LVGL writes per redraw. The
  panel itself follows the Color Mode setting (see
  [`architecture.md`](architecture.md#home-screen)); the renderers read it at
  `video_presenter_begin()`.
- **The clip is a rectangle.** Insets can cover several edges (a bar on top
  and bottom works), not a hole in the middle: one PPA block cannot skip its
  centre. MJPEG passes it to `jpeg_ppa_pipeline` as `out_clip` (strips outside
  it never reach PPA), and it disables the direct decode; H.264 and MPEG-2
  narrow the PPA input block (`PackedYuvScaler`).
- **The clip edge can leave a few black pixels.** A source pixel that straddles
  the edge is not drawn (YUV420 blocks round to even pixels too), so up to two
  scaled source pixels next to the bar stay black. Entering the mode clears a
  band that wide along each clip edge inside the video, so no stale picture is
  left there.
- **Ordering is what keeps the two writers apart.** `set_ui_insets()` waits for
  the worker to apply it, so the bar is only made visible once the video no
  longer draws there, and hiding waits for LVGL's last blit
  (`bsp_display_wait_draw()`) before handing the edge back. The bar is created
  hidden (`DisplayManagerConfig::visible`), otherwise its first render would
  land on whatever is on screen before the presenter starts.
- **Rotating recreates the bars.** Their logical size changes with the rotation,
  so `VideoPlayerScreen::rotate` deletes them, sets the presenter's rotation, creates
  new ones and applies their insets. The presenter applies a rotation between
  frames.
- **Applying insets has to leave framebuffer 0 on screen.** LVGL's partial
  blit writes into whichever framebuffer the panel driver last flushed and
  then flushes index 0 (`mipi_dsi.c`, `flush_framebuffer()`), so a bar drawn
  while the panel still shows framebuffer 1 or 2 is pushed off screen by that
  flush the moment it appears. Nothing looks broken from LVGL's side — the
  mode stays `Bars`, so the next tap toggles bars nobody can see and the
  player looks dead. The worker therefore presents inside the insets
  handshake even when the renderer cannot redraw the picture and the
  framebuffer still holds the frame before last, which nobody sees because the
  redraw replaces it a moment later.
- **The picture for a redraw comes from whoever still has it.** H.264 and
  MPEG-2 keep a decoded picture and the worker redraws from it. MJPEG decodes
  straight into the framebuffer and has nothing decoded to keep, so the worker
  only presents and `player_repaint()` follows with the packet the player
  pinned; while playing the player ignores the request, because the next frame
  covers the change anyway. Keeping that packet in the renderer instead would
  pin a ring chunk for as long as nothing is drawn (see
  [Reading](#reading-one-buffer-from-the-card-to-the-decoder)).
- **CPU writes to a framebuffer are synced to the cache immediately.** PPA and
  the JPEG decoder invalidate their output without writing it back, so a
  cleared area still in the cache would be lost.
- **The letterbox is cleared per framebuffer.** Each framebuffer has a
  "letterbox dirty" bit, set when the video rect, the rotation or the insets change. Just before a
  framebuffer is drawn into, the area outside the video rect (within the clip)
  is cleared and its bit dropped.
- **Everything the worker does holds `s_idle`**: drawing, redrawing,
  presenting, and applying new insets. `video_presenter_flush()` takes it
  before `discard()`, so a held picture is released before `reader_stop()`
  refills the free queues, and never twice. After a flush there is nothing to
  redraw until the next frame; a redraw request in that window blanks the
  picture.
- **Closing hands framebuffer 0 back.** The main display's partial blits go to
  the framebuffer on screen but flush index 0 (`display_manager`), so the
  presenter clears framebuffer 0 and shows it before the main display comes
  back.
- **`fb_num` is 3.** With two framebuffers, the next frame's output would compete
  with the one being scanned out.
- **The pipeline is created per session.** Its constructor purges the cache
  lines of the strip buffers, which LVGL has been writing through the CPU.

## Audio

- **PCM is copied before it goes out.** `bsp_audio_write` runs the BSP's DSP
  chain in place on the buffer it is given. `audio_out` copies PCM into its own
  buffer first, so a ring slot is never passed directly. Decoded audio already
  lands in that buffer.
- **The decoder follows the stream, not the container.** When a decoder
  reports a different rate or channel count (HE-AAC doubles the rate the
  AudioSpecificConfig states), `bsp_audio_open` is called again.
- **Decoders are reset on seek** (`audio_out_flush()`), so AAC and Opus do not
  carry state from before the jump. The audio task is parked by then.
- **AAC's SBR (HE-AAC) decoding is on only next to MJPEG.** On the device it
  took about a quarter of a core even for plain AAC-LC, which H.264 playback
  cannot spare; MJPEG is decoded in hardware and can. With H.264, an HE-AAC
  track plays its core only (half the rate, no high band); the rate change is
  handled like any other. The simulator's libavcodec ignores the setting.
- **AAC decides raw or ADTS from the first packet**, not from the container:
  AVI has several format tags for AAC and they are not used consistently. Raw
  AAC with no AudioSpecificConfig gets one built from the track's rate and
  channels (LC).
- **IMA ADPCM is decoded by `app/audio/ima_adpcm.cpp`**, on both targets.
  `esp_audio_codec`'s ADPCM decoder has no block-align setting, so it cannot
  follow the file's block size. Packets are split by `block_align`. Stereo
  blocks hold 4 bytes of the left channel, then 4 of the right, and so on (as
  in ffmpeg and libsndfile), not alternating nibbles. MS ADPCM is not
  supported.
- **Opus is MKV and MP4 only** (ffmpeg cannot mux it into AVI) and decodes at
  48 kHz. Only channel mapping family 0 (mono or stereo) is accepted. The
  OpusHead pre-skip is not applied on the device.
- **The playback position is PCM frames written divided by the sample rate.**
  It feeds the clock correction above.
- **The device MP3, AAC and Opus decoders are `espressif/esp_audio_codec`,
  pinned below 2.6.**
  2.6 and later refuse to build unless the ESP32-P4 is chip revision 3.0 or
  newer, and this board's P4 is older (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`).
  Check the silicon before raising the pin. The dependency is device-only.
- **The simulator decodes MP3, AAC and Opus with ffmpeg's libavcodec.** It is
  linked in `simulator/CMakeLists.txt`, and `flake.nix` provides it.
- **`MP3` uses `ESP_AUDIO_DEC_RECOVERY_PLC` on every packet and the others do
  not.** That is what MP3 has always been given; for Opus, PLC means "this
  packet was lost".

## Simulator

`simulator/verify/video_player.txt` goes Home → SD Card → `Movies/`, then exercises
poster, play, pause, hiding and showing the bar, seek, restart, loop and back
on `Movies/clip.avi`. It then plays `Movies/ffmpeg.avi`. It injects `imu rot90`
once the player is open, so the bar coordinates are the landscape ones. The bars
hide 4 s after the last touch while playing, so a script that plays longer than
that before tapping a control taps the video first to bring them back (unless the
clip has finished, which shows them again).
`simulator/verify/rotation.txt` plays `Movies/portrait.avi` (720x1280, the
direct path) and turns it through all four rotations, then checks the list
after going back. `simulator/verify/mkv.txt` turns `Movies/rotated.mkv`
through all four rotations and plays it at 270 (the direct path), exercises
play, seek and loop on `Movies/sample.mkv`, and plays `Movies/sample_pcm.mkv`
(640x360, PCM) to the end. The scripts tap list rows by position, so MKV
fixtures are named to sort after the AVIs. All of them are local fixtures in
the gitignored `simulator/sdcard`. To make an ffmpeg fixture (`size=720x1280`
for `portrait.avi`; `.mkv` for `sample.mkv`; `size=640x360:rate=10`,
`sample_rate=32000` and `-c:a pcm_s16le` for `sample_pcm.mkv`):

```sh
nix develop -c ffmpeg -f lavfi -i testsrc=size=1280x720:rate=15 \
    -f lavfi -i sine=frequency=440:sample_rate=44100 -t 4 \
    -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a libmp3lame -b:a 128k \
    simulator/sdcard/Movies/ffmpeg.avi
```

`rotated.mkv` is a landscape clip stored portrait, with the roll added on remux
(`-display_rotation` is an input option):

```sh
nix develop -c ffmpeg -f lavfi -i testsrc=size=1280x720:rate=15 \
    -f lavfi -i sine=frequency=440:sample_rate=44100 -t 4 -vf transpose=clock \
    -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a libmp3lame -b:a 128k upright_cw.mkv
nix develop -c ffmpeg -display_rotation 90 -i upright_cw.mkv -c copy \
    simulator/sdcard/Movies/rotated.mkv
```

- **Seek drags need a `wait` between `down` and each `move`.** A `move` in the
  same frame as the `down` never becomes a drag, and the seek silently does
  nothing.
- **`[mp3float] overread` after a seek is harmless.** ffmpeg's MP3 parser was
  handed a chunk from the middle of a frame and resynchronises.
- **`[opus] Could not update timestamps for skipped samples` is harmless.**
  libavcodec applies the OpusHead pre-skip and our packets carry no pts.

`simulator/verify/audio_codecs.txt` plays every file in `Test Audio/` (a root
directory named to sort after `Music/`, so the other scripts' rows do not
move): `aac.avi`, `aac.mkv` (with a seek), `aac_laced.mkv`, `adpcm.avi`,
`adpcm.mkv` (stereo), `opus.mkv` (with a seek) and `opus_laced.mkv`, each to
the end. Audio cannot be captured, so the check is the codec label and the
clock: a decoder that writes nothing holds the video at the first frame. Make
them with the first ffmpeg command above at `size=640x360:rate=10`, swapping
the audio codec (`-c:a adpcm_ima_wav`, `-c:a aac`, `-c:a libopus -ac 2` with
`sample_rate=48000`; `adpcm.mkv` uses
`aeval=val(0)|sin(t*3000)*0.5:c=stereo` after the sine). ffmpeg never laces,
and mkvtoolnix does not build from nixpkgs on macOS, so the `*_laced.mkv`
files were made by rewriting the ffmpeg MKVs with a throwaway script that
packs runs of up to four audio SimpleBlocks into one laced block (Xiph for AAC,
EBML for Opus) and drops `SeekHead` and `Cues`; they are therefore not
seekable. ffmpeg decodes them to the same samples as the originals.
`simulator/verify/mp4.txt` plays everything in `Test MP4/` (a root directory
sorting after `Test Audio/`), with seeks on the H.264, MP3, MJPEG and
non-interleaved files, the rotation check on `f_rotated.mp4`, and a loop on
`h_coarse.mp4`, then opens the empty `.mp4` placeholder at the root, which
fails (row 640, below the `Test MPEG2/` directory). The fixtures are the ffmpeg commands above at `size=640x360` with
`.mp4` output:

| file | how |
|---|---|
| `a_h264_aac.mp4` | H.264 baseline `-g 30`, `-c:a aac`, `-movflags +faststart`, 6 s |
| `b_h264_mp3_tail.mp4` | same with `libmp3lame` and no faststart (`moov` at the end) |
| `c_mjpeg.mp4` | MJPEG at `rate=10`, AAC (ffmpeg writes `mp4v`) |
| `d_opus.mp4` | H.264, `libopus -ac 2`, `sample_rate=48000` |
| `e_pcm.mov` | MJPEG at `rate=10`, `pcm_s16le` at 32 kHz (`sowt`) |
| `f_rotated.mp4` | as `rotated.mkv`, but H.264 + AAC at 1280x720 |
| `g_noaudio.m4v` | H.264 only, 3 s |
| `h_coarse.mp4`, `i_separate.mp4` | `a_h264_aac.mp4` rewritten into 1 s chunks, and into all video then all audio |
| `j_separate_big.mp4` | 12 s of `testsrc2` at `-qp 12`, rewritten like `i_separate`, so the tracks are 4 MB apart |

The rewritten files come from a throwaway script that copies the samples into
a new `mdat` and rewrites `stsc`/`stco`; ffmpeg decodes them to the same
frames as the original. The packets the demuxer returns for these files
(track, pts, size, keyframe) match `ffprobe -show_packets` per track, within
1 µs.

- **Do not measure frame rate here.** The host decodes JPEG in software, so a
  10 fps clip shows about 8-9 fps.

`z_h264_360p.mkv` and `z_h264_360p.avi` are H.264 Constrained Baseline
fixtures (keyframe every second), named to sort last so no script's rows move:

```sh
nix develop -c ffmpeg -f lavfi -i testsrc=size=640x360:rate=30 \
    -f lavfi -i sine=frequency=440:sample_rate=44100 -t 6 \
    -c:v libx264 -profile:v baseline -g 30 -pix_fmt yuv420p -c:a aac \
    simulator/sdcard/Movies/z_h264_360p.mkv
```

The `.avi` is the same command with `-c:a libmp3lame`.

`simulator/verify/mpeg2.txt` plays everything in `Test MPEG2/` (a root
directory sorting after `Test MP4/`) with a seek and to the end, in landscape.
The fixtures are `testsrc` at 640x360, 30 fps, 6 s, `-c:v mpeg2video -g 15
-b:v 1.5M`:

| file | how |
|---|---|
| `a_ibbp_mp3.avi` | `-bf 2`, `libmp3lame` |
| `b_ibbp_aac.mkv` | `-bf 2`, AAC |
| `c_ibbp_aac.mp4` | `-bf 2`, AAC, `-movflags +faststart` (ffmpeg writes `mp4v`, object type 0x61) |
| `d_ip_mp3.avi` | `-bf 0`, `libmp3lame` |
| `e_720p.mkv` | 1280x720, `-bf 2 -b:v 5M`, AAC, 3 s |

`simulator/verify/audio_player.txt` drives `AudioPlayerScreen` from `Music/`
(an existing root directory, so no other script's row positions move): it plays
`a_aac.m4a` in portrait with pause, a seek drag and the end of the file, then
`b_opus.m4a` in landscape with pause, repeat, a seek drag and the loop wrap,
and finally opens `c_alac.m4a`, whose codec is unsupported, to check that the
failure reaches the second line. Audio cannot be captured, so what the captures
prove is the clock, the state of the transport and the layout in both
orientations.

```sh
nix develop -c ffmpeg -f lavfi -i sine=frequency=440:sample_rate=44100:duration=6 \
    -c:a aac -b:a 128k simulator/sdcard/Music/a_aac.m4a
```

`b_opus.m4a` is `-c:a libopus` at `sample_rate=48000 -ac 2` with `-f mp4`
forced (the `.m4a` extension picks the `ipod` muxer, which refuses Opus), and
`c_alac.m4a` is `-c:a alac`.

The same script then plays `d_pcm.wav` (`-c:a pcm_s16le`), `e_adpcm.wav`
(`-c:a adpcm_ima_wav` at 22050 Hz), `f_mp3.mp3` (`-c:a libmp3lame -b:a 128k`,
with a seek to the end), `g_vbr.mp3` (`-c:a libmp3lame -q:a 4`, so ffmpeg
writes a Xing header) and `h_aac.aac` (`-c:a aac -f adts`), all 6 s of
`sine` at `-ac 2`, and ends on the empty `track.aac`, which fails.

`simulator/verify/media_tags.txt` checks the tag path: `Music/z_tag_v24.mp3`
(ID3v2.4 with a 600x600 JPEG cover), `z_tag_v23.mp3` (ID3v2.3 with a 640x360
PNG, which letterboxes inside the square), `z_tag_v1.mp3` (an ID3v1 block
appended by hand — ffmpeg's `-write_id3v1` writes nothing when the metadata
went into an ID3v2 tag), `z_tag.m4a` (`ilst` with `covr`) and `z_tag.wav`
(`LIST INFO`), then the Media Info panel of `Movies/z_tag.mkv`,
`Movies/z_tag.avi` and `Test MP4/z_tag.mp4`. All of them are remuxes of the
fixtures already there, named to sort last so no other script's rows move:

```sh
nix develop -c ffmpeg -f lavfi -i testsrc=size=600x600:rate=1 -frames:v 1 cover.jpg
nix develop -c ffmpeg -i simulator/sdcard/Music/f_mp3.mp3 -i cover.jpg \
    -map 0:a -map 1:v -c copy -id3v2_version 4 -disposition:v attached_pic \
    -metadata title="Night Drive" -metadata artist="The Tab Fives" \
    -metadata album="Pocket Symphony" -metadata track="3/12" -metadata date="2026" \
    simulator/sdcard/Music/z_tag_v24.mp3
```

The Matroska one also carries the cover as an attachment
(`-attach cover.jpg -metadata:s:t mimetype=image/jpeg`).
