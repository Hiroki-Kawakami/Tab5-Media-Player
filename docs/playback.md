# Playback

What plays today: MJPEG or H.264 (Constrained Baseline, up to 1280x720) in
`*.avi`, `*.mkv` and `*.mp4`/`*.m4v`/`*.mov` with PCM, MP3, IMA ADPCM or AAC
audio, plus Opus in MKV and MP4 (or no audio), full screen, with play/pause, restart, seek, loop and
volume. Picking one in the file browser opens `PlayerScreen`. The H.264 decoder
itself is described in [`h264.md`](h264.md).

The planned additions are audio-only files, images, playlists and playing
a directory in order. None of them exist yet.
Their names are in the layout so you can see where each would land. Only the
decisions that are expensive to reverse later were taken now.

## Layers

```
PlayerScreen        LVGL overlay, transport UI
  │ player_open(path) / play / seek(us) ...
Player              command task, state machine, media clock, pacing, loop
  │
Demuxer ──Packet──▶ ring ──▶ video_presenter ──▶ MjpegRenderer ─┬─ JPEG ▶ FB            (direct)
  (Avi/Mkv/Mp4)       │         placement, FB,  │                  └─ JPEG ▶ strips ▶ PPA ▶ FB (pipeline)
  │                   │         letterbox,      └─ decoder task ─▶ H264Renderer ─ h264_dec ▶ packed DPB
  │                   │         overlay ◀── ready queue ◀──────────────┘   ▶ PPA (YUV420) ▶ FB
media_buffer          │           └─▶ compose overlay ─▶ present
  (arena)             └──▶ audio_out (PCM / MP3 / ADPCM / AAC / Opus) ──▶ bsp_audio_write
```

| path | role |
|---|---|
| `components/media_buffer/` | aligned read-ahead into the arena, packets as views into it |
| `components/avi_demux/` | RIFF/AVI parsing, plain C with no app types |
| `components/mkv_demux/` | EBML/Matroska parsing, plain C with no app types |
| `components/mp4_demux/` | ISO BMFF/QuickTime parsing, plain C with no app types |
| `components/h264_dec/` | the H.264 decoder, plain C, host-testable (see [`h264.md`](h264.md)) |
| `app/media/` | `Demuxer` interface, `MediaInfo`/`Packet`, the AVI, MKV and MP4 adapters |
| `app/playback/` | `Player`: reader, audio and pacing tasks |
| `app/video/` | `video_presenter` (placement, framebuffers, overlay, decode/present stages), `VideoRenderer` and its MJPEG and H.264 implementations, the FreeRTOS hooks the decoder runs on |
| `app/audio/` | `audio_out`: BSP output, compressed audio decode, playback position; `ima_adpcm` |
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
composing the overlay and presenting. A `VideoRenderer` turns a packet into a
`VideoFrame` (`decode`) and puts a frame into a `RenderTarget` (framebuffer,
rotation, `n`, rect, source size) with `draw`. `draw(nullptr, …)` redraws the
held frame. MJPEG's `decode` only reads the header and the JPEG is decoded
inside `draw`; H.264's `decode` does the real work and `draw` is one PPA call.

- **The scale is `n/16` because PPA quantizes to 1/16** (8-bit integer and
  4-bit fraction). `n` is `floor(16 × min(fit/source))`, so clips are scaled up
  as well as down. The rect is `source × n / 16` rounded down, which is the size
  the driver produces, so centering matches what lands in the framebuffer.
  Strips are 16 rows, so every strip boundary scales to a whole row and the
  pipeline's check never rejects this scale. The simulator's PPA shim rounds
  instead of truncating and can be a pixel off.
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
  cannot be copied back because the overlay is already composed into it. One
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

**The player only opens paths.** It has no idea what comes next. Playing a
directory in order or a playlist becomes a layer that watches for `Finished`
and calls `player_open()` again. The screen is called `PlayerScreen`, not
`VideoScreen`, because audio files will use it too.

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
- **The player submits H.264 frames early.** `step_playing()` hands a frame
  over 120 ms before it is due (`kDecodeLeadUs`) together with its due time in
  `esp_timer` terms. The poster and anything submitted while paused carry a due
  time of 0, which means "now". Pausing therefore still shows the few frames
  already in the pipeline.
- **Late frames are decoded, not skipped.** A frame more than one interval late
  is still decoded (`present = false`) because later frames reference it,
  unless its NAL header says nothing references it (`nal_ref_idc == 0`,
  checked by the reader). A late frame is still shown if nothing has been
  shown for 200 ms, so a stream that runs just behind keeps moving on screen.
- **Too late means skipping to the next keyframe.** Past
  `max(500 ms, 5 intervals)` of lateness, or three intervals while the video
  ring is full, the player drops everything up to the next keyframe and shows
  that one whatever its lateness. The full-ring rule is what keeps audio
  going: the reader takes packets in file order, so a full video ring also
  stops audio, and the audio-corrected clock then slows down with it, which
  hid the lateness. A 720p clip that decodes at 10 fps played in slow motion
  before this.
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

## Deliberately not done yet

- **No output reordering.** Pictures are shown in decoding order. Baseline
  streams whose picture order count differs from it (the JVT `MR4/MR5_TANDBERG`
  streams do) decode correctly but show frames out of order. x264's baseline
  output never does this.
- **No fragmented MP4.** Files with `mvex`/`moof` are rejected.
- **No CABAC, B slices, interlace, weighted prediction or 8x8 transform.**
  They are rejected with "re-encode with -profile:v baseline"; see
  [`h264.md`](h264.md).

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
- **A view can wait on the player** (pins held by the presenter or the ring), so
  `reader_stop()` interrupts the buffer before waiting for the reader to park.
  An interrupted read rewinds the demuxer to the element it started on.
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
- **H.264 keyframes come from `idx1` (`AVIIF_KEYFRAME`).** The index keeps
  only keyframes as seek points, so `avi_demux_seek()` lands on the last
  keyframe at or before the target and reports that frame; the adapter derives
  pts from it. Without `idx1` a packet is a keyframe when its first slice NAL
  is IDR (type 5), and only `seek(0)` works. MJPEG ignores the flags.
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

- **Tracks.** The first video track must be `V_MJPEG` or `V_MPEG4/ISO/AVC`
  (its avcC `CodecPrivate` is copied and handed up raw). Audio is
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
  or `mp4v` whose esds says MJPEG (object type 0x6C, which is what ffmpeg writes
  for MJPEG in `.mp4`). Audio is `mp4a` with an AAC or MP3 object type (the esds
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
| `media_readahead` | 3 | 64 KB aligned read-ahead into the arena |
| `player` | 5 | commands, pacing, submit to the presenter |
| `video_presenter` | 6 | (MJPEG: decode →) PPA → compose overlay → present, core 0 |
| `video_decoder` | 2 | H.264 decode (parse, prediction, residual), core 0 |
| `h264_post` | 2 | H.264 deblocking, packing, frame writes, reference window, core 1 |
| `media_audio` | 6 | audio ring → `audio_out_write` |

Audio has the highest priority because a late audio write is audible and a
late frame is not. The presenter sits at 6 too, on core 0 with the decoder,
so it wakes on time while the decoder is busy; it spends most of its time
waiting for PPA.

The two H.264 workers sit below everything else because they are the only
tasks that use a whole core. At 720p they never block, and above the reader
(4), the read-ahead (3) and LVGL (4) they would starve all three. The decoder
also sleeps 20 ms after 500 ms without blocking, which keeps `IDLE0` fed for
the task watchdog. Taking a harness capture while a heavy clip plays still
trips the watchdog, since the capture itself runs on core 0 for seconds.

- **The overlay costs frame rate.** With the bar shown, every frame is also
  blended with it, and a 360p clip that plays at 30 fps with the bar hidden
  falls to about 20 fps on screen (the rest are decoded but not shown).

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
LVGL display is hidden. The controls are a separate LVGL display
(`DisplayPresentMode::Deferred`) along the bottom edge as the user sees it:
160 px in landscape, 240 px in portrait, where the button row wraps. The
presenter composes it over each picture before presenting.

- **Rotating recreates the bar.** Its logical size changes with the rotation,
  so `PlayerScreen::rotate` detaches it from the presenter
  (`video_presenter_set_overlay(nullptr)` waits for the worker), deletes it,
  sets the presenter's rotation and creates a new one. The presenter applies a
  rotation between frames and draws the held frame again.
- **The video sets the update rate.** LVGL only marks the overlay dirty on
  `LV_EVENT_RENDER_READY`, and the next frame picks it up. While paused, the
  presenter wakes every 100 ms and presents a dirty overlay on its own.
  Otherwise the Play button would never change.
- **`compose()` cannot erase, and neither can a render.** Both write only their
  own rect. Each framebuffer has a "letterbox dirty" bit. The bits are set when
  the video rect changes, the rotation changes, the bar is replaced, or the bar
  is hidden. Just before a framebuffer is drawn into, the area outside the video
  rect is cleared and its bit dropped. The framebuffer on screen is never
  touched. A full-panel video has nothing to clear. Hiding the bar also redraws
  the held frame (`video_presenter_repaint()`).
- **CPU writes to a framebuffer are synced to the cache immediately.** PPA and
  the JPEG decoder invalidate their output without writing it back, so a
  cleared area still in the cache would be lost.
- **Everything the worker does holds `s_idle`**: drawing, redrawing,
  presenting, and swapping the overlay. `video_presenter_flush()` takes it
  before `discard()`, so the held slot is released before `reader_stop()`
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

`simulator/verify/player.txt` goes Home → SD Card → `Movies/`, then exercises
poster, play, pause, hiding and showing the bar, seek, restart, loop and back
on `Movies/clip.avi`. It then plays `Movies/ffmpeg.avi`. It injects `imu rot90`
once the player is open, so the bar coordinates are the landscape ones.
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
fails. The fixtures are the ffmpeg commands above at `size=640x360` with
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
