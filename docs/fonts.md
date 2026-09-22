# Japanese text

The UI is Montserrat (LVGL's built-in) with NotoSansJP behind it for everything
Montserrat does not have. Only the two roles that ever show file names or tags
are chained: `LV_WIDGETS_FONT_BODY` (24 px) and `LV_WIDGETS_FONT_TITLE` (38 px).
`HEADING` (28 px) and `CAPTION`/`BUTTON` (20 px) are modal titles, modal bodies
and button labels — all fixed English strings today, so they stay Montserrat and
would show placeholder boxes if Japanese ever reached them.

## Why a resgen pack and not an `lv_font_t`

A 2542 glyph subset costs 276 KB at 24 px and 690 KB at 38 px as plain 2 bpp
bitmaps, and 230 KB / 438 KB compressed. Neither LVGL's `lv_font_fmt_txt` nor
its compressed variant (`LV_USE_FONT_COMPRESSED`, which decompresses and
`lv_malloc`s line buffers on *every* draw) keeps a decoded glyph around, so the
pack is generated in its own format and decoded by `app/packed_font.cpp`, which
caches decoded glyphs as A8 masks in PSRAM and hands them to LVGL through the
static-bitmap path (no per-draw allocation, no copy into a draw buffer).

Storing the 24 px pack raw (`"compress": false`) was tried and reverted: it costs
62 KB and buys nothing measurable. Scrolling a directory of Japanese file names
in the simulator decodes 358 glyphs against 3072 cache hits, 0.7 ms of decoding
in total — about 2 us per glyph — and never falls back to LVGL's draw buffer.

Only the native sizes are stored. Rendering another size by scaling a master was
measured (a 24 px glyph area-averaged from the 38 px master) and looks visibly
softer than the native 24 px raster, so the two sizes the UI actually uses are
each stored at their own size.

## The pack format

`resgen.py` emits `resgen_font_pack_t` (declared in the generated
`resources.h`): a sorted `uint16_t` codepoint table, a `resgen_glyph_t` per
glyph, and one bitmap blob. Codepoints are limited to the BMP; the subset needs
nothing above U+FF9F.

`resgen_glyph_t::bitmap` is the byte offset into the blob, with bit 31 set when
the glyph is RLE compressed. Glyphs that do not get smaller are stored raw
(packed `bpp` bit rows, no row padding), so the decoder handles both regardless
of what `compress` says.

The RLE is the scheme LVGL uses for its own compressed fonts (see `decompress()`
and `rle_next()` in `lv_font_fmt_txt.c`): every row is XORed with the row above
it, a value costs `bpp` bits, a value equal to the previous one switches to
repeat mode where each further repeat is a single 1 bit, a 0 bit ends the run,
and the 11th repeat is followed by a 6 bit count. The count runs out on a
value — not on a repeat — and the decoder does *not* re-enter repeat mode on
that value; encoders that get this wrong produce streams that are one pixel
short per long run. `resgen.py check <definition>` decodes every glyph back and
compares, and is the way to verify a change to either side of the codec.

The blob carries two zero bytes of slack at the end because the decoder reads a
24 bit window.

The pack also stores `max_ascent`/`max_descent` of the included glyphs. Noto's
own line metrics (1.45 em) are far taller than Montserrat's box, so the chain in
`app/ui_font.cpp` keeps Montserrat's metrics and grows them only far enough that
no kanji is clipped: 1 px at 24 px, 2 px at 38 px.

## The font file

`app/resources/fonts/NotoSansJP-subset.ttf` is the upstream variable font cut
down to `jp_glyphs.txt` with the weight axis kept, so `resources.json` picks the
weight (`wght: 500`, matching the Montserrat Medium LVGL builds its fonts from).
To regenerate it after editing the glyph list:

```sh
nix develop -c sh -c '$RESGEN_PYTHON -m fontTools.subset NotoSansJP-VariableFont_wght.ttf \
    --text-file=<glyph list without the # lines> \
    --output-file=app/resources/fonts/NotoSansJP-subset.ttf \
    --layout-features= --no-hinting --name-IDs="*"'
```

`jp_glyphs.txt` is the joyo kanji plus kana (including the halfwidth block),
fullwidth alphanumerics and the punctuation and symbols that show up in file
names. ASCII is deliberately absent: those codepoints always resolve in
Montserrat, which comes first in the chain, so a Noto copy would be dead weight.
Characters outside the list render as LVGL's placeholder box.

## Checking it

`simulator/verify/japanese.txt` walks into a directory of Japanese file names
and opens one, covering both sizes. It expects those files on the simulator's
SD card (`simulator/sdcard`, which is not in git, like every other verify
script's fixtures).
