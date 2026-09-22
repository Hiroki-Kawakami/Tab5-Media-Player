# Metadata cache

Tags, cover art, browser thumbnails and the pictures the image viewer shows are
produced on a background task and kept in `app/media/media_cache.*`, so no
screen ever parses a file or decodes a picture on the LVGL thread.
`app/media/media_probe.*` is the blocking half for playable files (open a
demuxer, take `MediaInfo`, close), `app/media/image_codec.*` turns encoded
pictures into pixels and back.

An image file needs the same four things a cover does — read it, decode it,
scale it, keep it — so it is the same cache with one more branch rather than a
second one. Everything below applies to both unless it says otherwise; see
[`images.md`](images.md) for what the viewer does with the result.

## Why a second arena

`Demuxer::open()` wants a `media_arena_t`, and the player holds the 4 MB one
for as long as a file is open — prefetching the next track while one plays
cannot share it. Header parsing never takes a `mb_view()` (checked across all
five demuxers: every view is on a read path), so a probe only needs enough
arena for `mb_read` and the index scans, and `media_player.cpp` allocates a
separate 512 KB one at boot. It is never freed, for the same reason the media
arena is not: a 512 KB contiguous PSRAM block is not guaranteed to come back
after the UI has been used for a while, and losing it would silently disable
thumbnails.

`mb_open()` used to demand about 2.75 MB, because its read-ahead depth was the
fixed `MB_DEPTH_CHUNKS`. The depth is now derived from the arena
(`min(16, ring_chunks / 2)`), which leaves the 4 MB arena at exactly the
numbers it had (ring 48, bounce 16, depth 16) and lets the probe arena through
(ring 6, bounce 2, depth 3).

## One image, many files

An album's tracks carry the same cover, so images are keyed by content, not by
path: the meta entry for a path stores only an `image_id`, so there is no
heuristic on tags or sizes — same bytes, same entry, whether or not the files
agree on the album name. An image file is hashed the same way, over a window of
the file itself, so two copies of one photograph decode once.

The id is a 32-bit FNV-1a over an 8 KB window 1 KB into the picture, paired
with the exact byte count. Hashing all of a 500 KB cover measured 20 ms per
file, which is most of what an info-only probe now costs; the header is what
the window skips because encoders make it identical across an album. Two
different pictures would have to share both a length and that window to
collide, and the cost of losing that bet is one wrong thumbnail.

An entry probed without its picture has no id yet — `has_cover` and
`cover_at` are known, the bytes are not. The id is filled in the first time
something actually reads them.

Dedup does not save the probe itself: every file is opened anyway for its
title. What it saves is decode, resize, encode and storage.

## Reading a tag

The picture is nearly all of a tagged file, so the tag reader walks the ID3
frame list over the file rather than pulling the tag into memory: it reads
each frame header, keeps the small text frames, and for a picture reads only
the mime/description prefix to work out where the bytes start. With
`want_cover` false it stops there and records the offset and length in
`cover_at`; with it true it reads the picture straight into the buffer that
becomes `CoverBytes`. Nothing is copied on the way, and an info-only probe of
a file with a 400 KB cover costs 25 ms instead of 133 ms.

Two things have to stay in one piece. Globally unsynchronised tags rewrite the
byte stream, so frame lengths stop matching file offsets and those fall back
to reading the whole tag; and mp4/mkv keep their pictures inside a parsed box,
so they have no offset to report. For them `cover_scanned` stays false when
the probe was told to skip pictures, which is how the cache knows that "no
cover" is not yet an answer and a full probe is still owed.

## Why reads are aligned to their file offset

FATFS copies the leading partial sector of a read through its own window and
then hands the rest to DMA. When the destination does not carry the same
64-byte phase as the file offset, every sector after that bounces through an
internal buffer instead: 6.4 MB/s against 11.3 MB/s, measured on the board
with 512 KiB reads. `mb_read_alloc()` over-allocates and starts the data at
the matching phase, and the windowed reader aligns its refills down for the
same reason.

That window starts at 8 KB and doubles while refills stay sequential. It used
to be a flat 64 KB, which made the 10-byte ID3 header peek cost a 64 KB read —
about 10 ms before anything had been parsed — while the frame scan that
follows still wants the big window.

## Nothing of this lives in internal RAM

The board boots with almost no internal SRAM to spare: the startup init
functions (the SDIO link to the C6 among them) take about 135 KiB of the
145 KiB that is free when the static constructors run, and the FreeRTOS timer
task's own stack is allocated out of what is left. An earlier version of this
cache put its containers in `.bss`/`.data`, which cost 391 bytes and was enough
to make `vApplicationGetTimerTaskMemory` fail — the board panicked before
`app_main`. So:

- the whole cache is one `CacheState` allocated from PSRAM in
  `media_cache_init()`, leaving a pointer behind in `.bss`;
- every container in it takes `PsramAllocator`
  (`app/media/psram_allocator.hpp`), because `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`
  is 16 KiB and the default allocator would put every map node, key and entry in
  internal RAM;
- the entries hold fixed-size UTF-8 like `media_tags_t` does, so a cached title
  is not a heap string either;
- the worker's 8 KiB stack is allocated with `MALLOC_CAP_SPIRAM`
  (`xTaskCreatePinnedToCoreWithCaps`). With it in internal RAM the player could
  no longer place the audio task's stack there and fell back to PSRAM, which is
  what that path is measured not to like.

8 KiB is the size the demuxers need, not a guess: 4 KiB overflowed the task on
the first MP3 it probed, and a PSRAM stack overflowing writes into the
neighbouring heap block rather than tripping the stack guard, because a 4 KiB
buffer in one frame steps straight over it. The decoder task gets 16 KiB
instead, because it runs the image decoders rather than a header walk;
`meta stats` prints what is left of both.

## Prefetching gives way to playback

Internal SRAM, not PSRAM, is what runs out: after boot about 74 KiB is free and
opening a file for playback takes 25 KiB of it (the reader, its `media_buffer`
and the audio codec). A directory of a hundred MP3s probed back to back walked
that down until `mb_open()` could not create its semaphores any more, and the
file the user tapped failed with "cannot open the file". So:

- `image_codec` asks for a JPEG engine only when there is a clear block of
  DMA-capable internal RAM free. `jpeg_new_decoder_engine()` and
  `jpeg_new_encoder_engine()` dereference their half-built handle on the
  out-of-memory path (IDF v6.1), so running the heap down to them is a panic,
  not a failed call. Without an engine the decode falls back to the software
  path and the artwork is simply not re-encoded;
- a probe opens the file with `media_arena_t::direct`, which is what the
  metadata arena is marked as: no read-ahead task at all. Read-ahead exists to
  keep video decoding fed; header and tag reads only need `mb_read`, which the
  direct path serves from a 64 KiB window over the bounce area so an index scan
  is still one read per window rather than one per entry. `mb_view()` fails in
  that mode, which is fine because no demuxer takes a view while parsing
  headers. The playing file's own read-ahead is untouched, and its task stack
  now comes from PSRAM;
- `es_audio_demux` keeps its 4 KiB resync scan buffer in its own (PSRAM)
  struct instead of on the caller's stack, which is what overflowed the
  worker's first 4 KiB stack;
- the browser prefetches `kPrefetchLimit` files beyond the ones on screen, not
  the whole directory, and `AudioPlayerScreen` drops the idle queue when it
  opens so the file being played is never behind a hundred probes.

## Three stores, three budgets

| store | key | holds | budget |
|---|---|---|---|
| meta | path | tags, duration, codecs, picture size, `image_id` | 512 entries |
| raw | `image_id` + box | RGB565 pixels | 1 MB |
| jpeg | `image_id` + box | the resized image, re-encoded | 4 MB |
| decoded | `image_id` + box | pixels expanded from `jpeg` | 2 MB |
| view | `image_id` + box | the same, for a screen-sized box | 6 MB |

A request carries the box it has to fit inside, not a side: a thumbnail and the
artwork are squares (56 and 552 px), but a picture on screen is 720x1280 or
1280x720 and fitting it into a square of the longer side would be a different
picture. Small images (box up to 128 px) are stored as pixels and handed
straight to LVGL; anything larger is re-encoded with the JPEG hardware at
quality 90, which turns a 608 KB artwork into 40-60 KB and makes "the last
80 albums" affordable. Expanding one again is a few milliseconds, and only the
worker ever does it.

The budgets are separate because the rebuild costs differ by three orders of
magnitude: with one pool, scrolling a long folder would evict the artwork of
the track that is playing. `view` exists for the same reason one step up: one
screen-sized picture is 1.8 MB at RGB565, so a few of them in the `decoded`
pool would evict every album the audio screen has seen. The split is by box
size (over 640 px goes to `view`), not by who asked. Eviction inside a store drops orphans first (no meta
entry references the id any more), never touches an entry the UI still holds a
`shared_ptr` to, and otherwise takes the least recently used. Dropping a cache
slot is always safe — the pixels live until the LVGL object that shows them is
deleted.

Probe failures are cached as well. Without that, a broken file is reopened on
every pass of the browser.

## Image files

An image file is its own picture, so there is no demuxer and no tag walk:

- **The probe reads a 128 KB window**, hashes the id from it and takes the
  picture's size from the JPEG SOF or the PNG IHDR if it is in there. Whether
  the size was found says nothing about whether the file can be decoded — a
  camera JPEG puts EXIF, a thumbnail and an ICC profile before its frame
  header, and 200 KB of that is ordinary — so only the format has to be
  recognised for the entry to count as a picture, and the decode fills the size
  in afterwards. Gating the entry on the frame header instead is what made
  large JPEGs fail while PNGs, whose IHDR is always at byte 16, kept working.
  The same window is where the EXIF tags are parsed from, so the Media Info
  panel costs no further reads; see [`images.md`](images.md#what-exif-adds).
- **The picture itself is read by the decoder stage, not the reader.** A cover
  is a few hundred KB, so handing the bytes from stage one to stage two costs
  nothing; a photograph is megabytes, and with the reader already fetching the
  next one, two encoded pictures sit in PSRAM at once. Together with the stores
  and the two arenas that was enough to run the board out of memory on a folder
  of large JPEGs — the decode failed, on a file that had opened a minute
  earlier. Stage two opens the file itself instead: one encoded picture at a
  time, paid for with the overlap between a read and the decode before it.
- **Only a baseline JPEG is read into memory**, up to 8 MB, because the
  hardware decoder is the one thing that wants the stream as one contiguous
  block. PNG and progressive JPEG are pulled off the card a row at a time,
  which asks nothing of the heap: a 7.5 MB PNG used to fail on a heap with
  15 MB free but no block bigger than 7.4 MB, for a buffer it never needed.
  When the block for a baseline JPEG cannot be found either, the decode falls
  back to the same streaming path instead of failing.
- **A failed decode is recorded as a flag on the entry**, not by taking the
  picture away from it. Cover art can afford "this file has no picture" because
  there is one size; an image file is asked for at several boxes, and clearing
  the entry's id on the box that failed loses the sizes that decoded — which is
  what put an error message over a picture that was on screen. The flag also
  carries whether the failure was memory, which is the difference between "too
  large to decode" and "cannot decode the image".
- **A picture that ran out of memory is not asked for again**, at any size: the
  coefficients a progressive JPEG needs do not depend on the box, so every
  other size would fail the same way after the same read.

## Withdrawing a running job

`media_cache_cancel()` drops a token's queued requests, but the viewer also has
to be able to leave the picture it is no longer showing: a swipe during a decode
must not make the next picture wait for it. Each stage therefore publishes the
token of the job it is on, and a cancel of that token raises the stage's flag;
the read loop and the software decode both poll it, and the hardware decode is
one blocking call that is left to finish. A withdrawn job reports nothing, so
the observer is not woken for a picture nobody is waiting for any more.

The token is published and the flag cleared under the cache lock, together, so
a cancel that arrives while the worker is picking up its next job either
catches the job it named or nothing at all.

## Pixel format

Thumbnails are always RGB565: converting 56x56 at draw time is nothing, and
making them follow the panel would mean throwing all of them away (and
re-probing every file) whenever Color Mode changes. The artwork follows
`bsp_display_get_pixel_format()`, because a 552 px image converted per pixel by
LVGL is not. Since the artwork is stored as JPEG, a format switch only
invalidates the `decoded` store — `media_player_set_display_pixel_format()`
calls `media_cache_invalidate_decoded()` and the few entries are expanded again
from bytes that did not care.

RGB888 pixels are in LVGL's B, G, R order (`IMGF_PIX_BGR888`, added to
image_framework for this) and IDF's `JPEG_ENCODE_IN_FORMAT_RGB888` is
`ESP_COLOR_FOURCC_BGR24`, so the same buffer feeds the panel and the encoder
with no swap.

## Decoding a cover

Baseline JPEG goes through `jpeg_ppa_pipeline` (Layer 2 of
jpeg_decode_enhanced): the hardware decoder writes 16-row strips and PPA SRM
scales each strip down as it lands. Both targets take this path — the host
build is backed by image_framework, so the simulator exercises what the board
runs. For PNG, a progressive JPEG or a picture wider than the strip buffers can
take, the fallback streams rows out of image_framework's decoder through the
resizer, which never materialises the full picture.

The reason for PPA is that the decode was never the cost. On a 700x700 cover
the hardware decode is 11 ms and the software box-downscale to 56x56 was
135 ms: the resizer walks every source pixel with a 64-bit accumulate, which
RV32 does not have. PPA reads the same pixels as DMA instead, and a 700x700
cover now costs 36 ms of pipeline plus 8 ms of box.

PPA cannot land on the wanted size by itself: its scale factors are quantized
to sixteenths and bottom out at 1/16, so 700 -> 56 is not expressible. The
pipeline therefore runs the smallest sixteenth that still overshoots the
target (2/16 -> 87x87 for a 700px cover) and the existing box resizer covers
the rest. That last step also does the pixel-format conversion, which is why
the strips and the PPA output stay RGB888 whatever the panel wants.

PPA scales by interpolating, not by averaging, so a 1/8 step samples 2x2 out
of every 8x8 block. Thumbnails come out slightly crisper and noisier than the
old full box filter. Cascading halvings through PPA would average properly if
that ever matters more than the 3x.

The two strip buffers start at 64 KB of PSRAM, which is a 1365 px wide JPEG
(16 rows x 3 bytes must fit one buffer), and grow in 64 KB steps with the
widest picture seen — a 3000 px photograph wants 144 KB each. They are capped
at 4096 px wide; anything wider falls back to software. Doubling 64 KB to
128 KB for covers was measured and changed nothing — PPA is bound by reading
the strips out of PSRAM, not by per-strip overhead. Internal RAM would be the
fast place for them, but 2 x 34 KB is most of what the board has free and the
video path already claims that budget.

`image_header()` parses the JPEG frame header and the PNG IHDR; a JPEG that is
not baseline is marked as such there, so the hardware path is never tried for a
picture it cannot take.

A progressive JPEG is the expensive case: the hardware cannot decode one at
all, and image_framework has to hold int16 coefficients for the whole frame
(about 3 bytes per pixel at 4:2:0) before it can emit a single row. A 5 Mpx
wallpaper therefore wants some 15 MB, which the heap has in total but, with the
arenas and the stores in it, rarely in one piece — the decoder allocates those
coefficients one block row at a time for that reason.

The decoder and encoder engines are created on first use and released when the
worker stops: browsing a folder without covers, or playing audio, holds no JPEG
hardware and no internal RAM.

## Stopping for the video player

`VideoPlayerScreen::onEnter()` calls `media_cache_stop()` before it takes the
shared SRAM, and `onExit()` calls `media_cache_start()` after it gives it back.
Stop joins both worker tasks (2 s budget each), which frees their stacks and
the JPEG engines, and drops the raw, jpeg and decoded stores. The meta entries stay:
they are text, a few hundred KB at the cap, and keeping them is what makes the
browser instant on the way back. `HomeScreen::onAppear()` lets the visible
`FileBrowserPage` ask again for the images that were dropped.

## Why the results are polled

The worker never takes the LVGL lock. `media_cache_stop()` runs on the LVGL
thread while it holds that lock, so a worker blocked in `lv_lock()` would
deadlock the join. Completed paths are appended to a list instead, and a 100 ms
`lv_timer` created by `media_cache_start()` hands them to the observers. The
same reason makes `media_cache_resolve()` safe: it spins on the LVGL thread
waiting for the cache map, not for the worker to reach the UI.

`media_cache_resolve()` is what keeps the audio screen from showing a file name
and then replacing it with the title. The browser has usually prefetched the
row already, so the call returns from the cache; the timeout only matters for a
file nothing has looked at yet. The artwork stays asynchronous, because a
placeholder that fills in changes no text.

## Scheduling

Requests carry a priority: `Blocking` (something is waiting for it), `Visible`
(rows on screen), `Idle` (the rest of the directory, the next track). The
reader takes the highest queue first and, while the player is playing, waits
200 ms between requests so prefetching cannot take the card away from playback.
A token identifies the requester; `media_cache_cancel()` drops its queued
requests when a page scrolls away or a screen leaves.

The work runs as two tasks, because reading the card and decoding a picture
share nothing: the reader produces a `DecodeJob` and the decoder turns it into
pixels while the reader is already on the next file. One job may be queued
between them — enough to hide the decode behind the read without holding a
third picture in PSRAM.

`FileBrowserPage` asks for the visible range twice: once for tags alone, then
for tags and thumbnails. Since the queue is FIFO within a priority, every row
gets its title before any picture is read, which is what makes a folder look
answered while the thumbnails are still arriving.

## What is not there

Video thumbnails are only the cover art a file carries. Decoding a frame of
H.264 or MPEG-2 needs the decoder's work arena, which is the shared SRAM the
main display renders into while the browser is on screen — the two cannot be up
at once. A first frame of MJPEG would work, but only for MJPEG, which is not
worth a second code path. `tools/converter-rs` therefore writes a 320 px
picture from a few seconds in as cover art (`covr` in MP4, an attachment named
`cover.jpg` in MKV), which this cache reads like any other; see
[`converter.md`](converter.md#thumbnails).

The browser also only prefetches audio files, so a video's thumbnail is read
when its row comes into view rather than ahead of it.

## Harness

`meta drain` waits until the queues are empty and the results have been
dispatched; `meta stats` logs the entry and byte counts of the five stores and
what is left of PSRAM.
`simulator/verify/thumbnails.txt` uses both.
