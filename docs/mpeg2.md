# MPEG-2 decoder

`components/mpeg2_dec` is an MPEG-2 video decoder written for this player from
ISO/IEC 13818-2, in the same style as [`h264.md`](h264.md): plain C that builds
and runs on the host, one PIE assembly file, pictures stored in PPA's packed
layout. How the player drives it is in [`playback.md`](playback.md#mpeg-2).

## Scope

Simple and Main profile, 4:2:0, 8-bit, progressive frame pictures, tuned for
360p and accepted up to the H.264 limits (3600 macroblocks, each side at most
1280). I/P/B pictures, open and closed GOPs, both scans, both intra VLC tables,
nonlinear quantiser scale, every `intra_dc_precision`, loaded quantiser
matrices and concealment motion vectors are supported.

Rejected with a message: MPEG-1 (no sequence extension), 4:2:2/4:4:4,
scalable extensions, field pictures, and field prediction, dual prime or field
DCT inside frame pictures.

- **Interlace is refused per macroblock, not per picture.** A frame picture with
  `frame_pred_frame_dct = 0` is allowed to use field tools but does not have to,
  and ffmpeg's encoder writes that flag (and `progressive_frame = 0`) whenever
  `-alternate_scan` is on, for streams that never use a field tool. Such
  pictures decode exactly; the first macroblock that actually signals field
  prediction or field DCT fails the stream. Real interlaced material hits that
  within the first few pictures.
- **Field pictures are not decoded because of the packed layout.** Packed row
  `2k` holds U line `k` and row `2k+1` holds V line `k`, so a top field's chroma
  lives partly on bottom-field rows; writing one field would be a
  read-modify-write of the other field's rows in PSRAM.

## Memory

Pictures are `O_UYY_E_VYY` frames in one PSRAM block: two anchors, the picture
being decoded, the held pictures and one spare. B pictures are never referenced
but still go to PSRAM, because PPA reads them from there.

The shared 240 KiB SRAM holds the VLC tables (20 KB; the DCT tables carry the
sign bit, which is what makes them 10-bit rooted) and one context per worker:
a 4 KB bitstream chunk, planar row buffers for one macroblock row, reference
fetch and bi-prediction temporaries. 23 KB per worker at 640 wide and 38 KB at
1280, so most of the block is still free.

- **Rows are packed straight into the PSRAM frame.** Packing into an SRAM row
  and copying it with `memcpy` measured 5% slower on real content; the SRAM row
  was only a second pass over the same bytes. The pack kernel writes whole
  32-pixel blocks, so when the width is not a multiple of 32 the last block goes
  through a 48-byte scratch and only its real bytes are copied: writing the whole
  block ran into the next line, which is another worker's row or, for the last
  row, the next frame of the pool.
- **Rows never share a cache line between workers.** A macroblock row is
  `24 × width` bytes, a multiple of 64 for any width that is a multiple of 16.

## Two cores, one row each

Unlike H.264 there is no deblocking and no intra prediction across
macroblocks, and an MPEG-2 slice never crosses a macroblock row. Rows of a
picture are therefore independent once its references exist. `decode()` collects
the picture's slices, then the calling core and one worker (`mpeg2_rows`, core
1, through `vdec_threads_t`) claim rows from an atomic counter and each does
everything for its row: parse, inverse quantisation, IDCT, prediction and pack.
The only synchronisation is waiting for the other core at the end of the
picture. Missing or broken macroblocks are concealed with the co-located
macroblock of the newest anchor (grey without one).

The second core buys less than one would expect (17.9 to 13.6 ms on the first
real 360p clip) for the reason [`h264.md`](h264.md#psram-access-measured)
spells out: most of a frame is PSRAM traffic, reference reads and the frame
write, and the two cores share the bus.

## Output order and timestamps

Anchors are held until the next anchor or until the temporal reference shows
nothing can come before them: an anchor is released right away when its
`temporal_reference` is the next one after the last output in the same GOP,
when it is picture 0 of a new GOP, or when the sequence is `low_delay`. So a
stream without B pictures has no reorder delay at all, and IBBP streams wait
for exactly the B pictures that precede the anchor.

- **After a restart the first anchor must be an I picture**, and B pictures
  that need a forward reference that was never decoded (open GOP, or a GOP with
  `broken_link`) are dropped. B pictures of a closed GOP are decoded with only
  the backward reference.
- **A loop wrap does not flush.** The first GOP of a file is closed in practice,
  so its B pictures do not look at the end of the file.

AVI stores packets in decode order and has no timestamps; how the adapter
recovers presentation times is in [`playback.md`](playback.md#mpeg-2).

## IDCT and bit exactness

MPEG-2 only specifies the IDCT's accuracy (IEEE 1180), so the output of two
conforming decoders differs. This one implements the same integer arithmetic as
ffmpeg's `simple` IDCT (row pass with a DC shortcut, `>> 11`; column pass with
the `W4 × 32` rounding term, `>> 20`), so every output can be compared byte for
byte with `ffmpeg -idct simple`. The encoder side of ffmpeg reconstructs with the
same IDCT, so test streams do not drift either.

`src/idct_p4.S` does both passes with `esp.vsmulas.s16.qacc` against constant
coefficient vectors, which needs no transpose: the row pass accumulates one
output row per QACC, the column pass one output row of the block. Facts:

- **It is exact except where the C version overflows.** QACC is 64 bits and
  `srcmb` saturates, while the C code (like ffmpeg) keeps the row pass in
  `int16_t` and wraps. They differ only for blocks with several coefficients
  near ±2047; `mpeg2idcttest` found 0 mismatches in 300000 random blocks with
  coefficients up to ±1024 and a handful per 100000 at ±2047.
- **The DC shortcut has to be reproduced.** ffmpeg's row pass returns
  `row[0] << 3` for a row with no AC, which is not what the full formula gives;
  the parser keeps a per-row AC mask so the kernel can take the same branch.
- **`srcmb` rounds by `vxrm`**, which is left at its default (floor), matching
  C's `>>` after the explicit rounding term.
- **Mismatch control makes blocks look dense.** Coefficient 63 is toggled
  whenever the coefficient sum is even, so about half of all blocks have row 7
  set; sparse-row tricks in the C column pass measured nothing.

## Verifying

```sh
nix develop -c cmake -S components/mpeg2_dec/test -B components/mpeg2_dec/test/build -G Ninja
nix develop -c cmake --build components/mpeg2_dec/test/build
components/mpeg2_dec/test/build/mpeg2_dec_test clip.m2v out.yuv
nix develop -c ffmpeg -idct simple -i clip.m2v -fps_mode passthrough -f rawvideo -pix_fmt yuv420p ref.yuv
cmp out.yuv ref.yuv
```

`MPEG2_THREADS=1` runs the two-worker path with pthreads, `MPEG2_HASH=1` prints
the per-frame FNV-1a the device bench prints, `MPEG2_VERBOSE=1` prints each
access unit, `-DMPEG2_SANITIZE=ON -DCMAKE_C_COMPILER=/usr/bin/clang` builds with
UBSan.

- **Material**: ffmpeg `mpeg2video` encodes of `testsrc2`, `mandelbrot` and real
  footage (re-encoded from the H.264 regression clips): I only, IP, IBBP, three
  B frames, closed GOP, `-intra_vlc 1`, `-alternate_scan 1`,
  `-non_linear_quant 1 -qmax 28`, `-dc 10/11`, custom matrices, `-q:v 1/2`,
  scrolling and rotating motion, 176x144, 352x198, 640x360 and 1280x720. All
  match, single and threaded. ffmpeg refuses `-flags +cgop` unless
  `-sc_threshold 1000000000` is given too.
- **Rejection**: `+ildct+ilme`, `+ildct`, field-order 720x480, `mpeg1video`,
  `yuv422p` and 1080p must each fail with their message.
- **Corrupt input**: flipped, overwritten and deleted bytes under UBSan, with
  and without threads, must neither crash nor hang (over 2000 runs clean).

On the device, `mpeg2bench` decodes a clip embedded at build time:

```sh
nix develop -c idf.py -C esp32p4 -B esp32p4/build-bench-m2 \
    -DMPEG2_BENCH_CLIP=/path/to/clip.m2v -DMPEG2_DEC_PROFILE=1 -p <port> flash
```

`mpeg2bench <loops> [hash] [single] [show] [present] [fps=N]` works like
`h264bench`; the profile shares are summed over both cores, so they add up to
about 200%. `present` gives each packet a due time from its temporal reference,
because due times in decode order make every anchor look late. `mpeg2idcttest
<blocks> [max level]` compares the PIE IDCT with the C one.

## Where the time goes

Device, both cores, PIE IDCT, bit-exact with the host:

| clip | decode |
|---|---|
| 640x360 real footage, IBBP, 0.5 Mbit/s | 11.8 ms (85 fps) |
| 640x360 `mandelbrot`, IBBP, 4 Mbit/s | 19.0 ms (53 fps) |
| 720x1280 real footage, IBBP, 1.7 Mbit/s | 43.5 ms (23 fps) |
| 1280x720 `testsrc2`, IBBP, 6 Mbit/s | 48.1 ms (21 fps) |

The first clip plays at 30 fps through `video_presenter` with no frame hidden.
On real footage reference fetches are about 40% of each core and the frame
write about 25%; on the 4 Mbit/s clip VLC decoding is about 20% and the IDCT 8%.
The PIE IDCT took the heavy clip from 23.7 to 20.5 ms, keeping the coefficient
loop's state in registers (the byte stores of the row masks aliased the bit
position) and folding the sign bit into the tables took it to 19.6.
