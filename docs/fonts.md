# Japanese text

The UI is Montserrat (LVGL's built-in) with NotoSansJP behind it for everything
Montserrat does not have. Only the two roles that ever show file names or tags
are chained: `LV_WIDGETS_FONT_BODY` (24 px) and `LV_WIDGETS_FONT_TITLE` (38 px).
`HEADING` (28 px) and `CAPTION`/`BUTTON` (20 px) are modal titles, modal bodies
and button labels — all fixed English strings today, so they stay Montserrat and
would show placeholder boxes if Japanese ever reached them.

## Why a resgen pack and not an `lv_font_t`

The 7141 glyph subset costs 783 KB at 24 px and 1410 KB at 38 px as 2 bpp packs,
tables included — about 17 per cent under the same bitmaps stored raw, and the
reason the factory partition went from 4M to 6M. Neither LVGL's
`lv_font_fmt_txt` nor its compressed variant (`LV_USE_FONT_COMPRESSED`, which
decompresses and `lv_malloc`s line buffers on *every* draw) keeps a decoded
glyph around, so the pack is generated in its own format and drawn by
esp-devkit's `PackedFont`, which caches decoded glyphs as A8 masks in PSRAM
(format and decoder: `esp-devkit/docs/resgen.md`).

Storing the 24 px pack raw (`"compress": false`) was tried and reverted: it
costs the best part of 150 KB and buys nothing measurable. Scrolling a directory
of Japanese file names in the simulator decoded 358 glyphs against 3072 cache
hits, 0.7 ms of decoding in total — about 2 us per glyph — and never fell back
to LVGL's draw buffer.

Only the native sizes are stored. Rendering another size by scaling a master was
measured (a 24 px glyph area-averaged from the 38 px master) and looks visibly
softer than the native 24 px raster, so the two sizes the UI actually uses are
each stored at their own size.

## Pack metrics

Codepoints in a pack are limited to the BMP; the subset needs nothing above
U+FF9F.

Noto's own line metrics (1.45 em) are far taller than Montserrat's box, so the
chain in `app/ui_font.cpp` keeps Montserrat's metrics and grows them by the
pack's `max_ascent`/`max_descent` only far enough that no kanji is clipped:
1 px at 24 px, 2 px at 38 px.

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

`jp_glyphs.txt` is all of JIS X 0208 — the non-kanji rows 1–8 (symbols,
fullwidth alphanumerics, kana, Greek, Cyrillic, box drawing) and every kanji
(level 1 and level 2, 6355 of them) — plus the NEC row 13 specials of CP932
(⑪, Ⅳ, ㈱, №), Latin-1 Supplement, halfwidth katakana, the few kana and
fullwidth forms outside JIS X 0208, and a short hand-picked line of symbols
that titles use but no JIS row has (♡, the en and em dashes, ♫, ⅰ). Symbols are taken by whole rows rather than
picked one by one because titles use the odd one (∞, 々, Ω, Д in kaomoji), and
Latin-1 because the built-in Montserrat stops at ASCII, so é or ö
in a tag would otherwise be a box. ≒ (U+2252) is the one JIS X 0208 symbol
Noto Sans JP does not have, so it is left out. The joyo kanji alone were
tried first and were not enough for a music library: 煌, 凛, 絆, 綺, 薔薇,
檸檬 and friends are all level 2, and titles and artist names use them freely.
ASCII is deliberately absent: those codepoints always resolve in Montserrat,
which comes first in the chain, so a Noto copy would be dead weight. Characters
outside the list render as LVGL's placeholder box.

## Decomposed kana

File names written by macOS and titles from some senders arrive in NFD.
`PackedFont` composes kana followed by U+3099/U+309A (see
`esp-devkit/docs/resgen.md`). Sorting still compares raw bytes, so NFD and NFC
names do not interleave.

## Checking it

`simulator/verify/japanese.txt` walks into a directory of Japanese file names
and opens one, covering both sizes. It expects those files on the simulator's
SD card (`simulator/sdcard`, which is not in git, like every other verify
script's fixtures).
