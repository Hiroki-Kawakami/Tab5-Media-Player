# Resources (fonts, icons, images)

`tools/resgen/resgen.py` turns a JSON definition plus source assets (SVG, TTF,
PNG/JPEG) into LVGL v9 C sources at build time. Only the sources are checked in;
the generated `.c`/`.h` live under the build directory.

The tool is meant to move into esp-devkit once it has been used for a while and
the definition format has settled. Until then it stays here and nothing in it
may depend on this app.

## Usage

`app/CMakeLists.txt` picks up `app/resources/resources.json` if it exists and
calls `resgen_add_resources()` from `tools/resgen/resgen.cmake`. App code then
includes `resources.h`:

```cpp
#include "resources.h"
lv_obj_set_style_text_font(label, &icon_36, 0);
lv_label_set_text(label, ICON_SETTINGS);
lv_image_set_src(img, &picture);
```

Python runs from `$RESGEN_PYTHON` (freetype-py, pillow, resvg-py), set by the
project `flake.nix`. It is a separate interpreter from ESP-IDF's
`IDF_PYTHON_ENV_PATH` so the two environments never collide on `python3`.

The generator can also be run by hand, e.g. to inspect output:

```sh
nix develop -c sh -c '$RESGEN_PYTHON tools/resgen/resgen.py all app/resources/resources.json /tmp/out'
```

## Definition

Plain JSON (no comments). Paths are relative to the JSON file. Every `font`
and `image` key becomes a global C symbol, so keys must be C identifiers and a
name cannot be both a font and an image.

```json
{
  "font": {
    "icon_36": {
      "size": 36,
      "icon": { "ICON_SETTINGS": "settings.svg", "ICON_INFO": "sdcard.svg" }
    },
    "icon_24": {
      "size": 24,
      "bpp": 2,
      "icon": { "ICON_INFO": "sdcard-24.svg" }
    },
    "noto_sans_24": {
      "size": 24,
      "font": "NotoSansJP.ttf",
      "variation": { "wght": 500 },
      "glyph": ["abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ"],
      "fallback": "icon_24"
    }
  },
  "image": {
    "picture": { "file": "picture.jpg", "format": "RGB565", "width": 320 }
  }
}
```

Font keys:

| key | |
|---|---|
| `size` | pixel size (required) |
| `bpp` | 1, 2, 4 or 8; default 4 |
| `icon` | `{ NAME: file.svg }` — makes an icon font (exclusive with `font`) |
| `font` | TTF/OTF file — makes a text font (exclusive with `icon`) |
| `glyph` | array of strings; the union of their characters is included. Omitted: every mapped glyph in the font |
| `variation` | variable-font axis values, e.g. `{"wght": 700}`; omitted axes use the font default |
| `fallback` | a generated font name or any `lv_font_t` symbol (e.g. `lv_font_montserrat_24`) |

Image keys: `file` and `format` are required. `format` is one of `RGB565`,
`RGB565A8`, `RGB888`, `XRGB8888`, `ARGB8888`, `L8`, `A8` (`A8` needs a source
with an alpha channel). `width`/`height`: neither keeps the source size, one
scales the other by aspect ratio, both stretch.

## Icon fonts

- Each icon name gets one code point, assigned in order of first appearance
  across all icon fonts starting at U+E000. The same name in several fonts
  shares the code point, so `ICON_INFO` works with every size; the file may
  differ per font (a hand-tuned small variant, say). Names are `#define`d once
  in `resources.h` as UTF-8 string literals.
- U+E000–U+EFFF is used because LVGL's built-in `LV_SYMBOL_*` sit at U+F000 and
  up, so a Montserrat fallback never shadows an icon.
- The SVG is rendered with resvg at `size` px tall (width by viewBox aspect)
  and only the alpha channel is kept, so `currentColor` strokes/fills work and
  the label's text color applies. Tabler and Lucide SVGs render as-is.
- `line_height` is `size` and the baseline is the bottom of the icon, so an
  icon reached through `fallback` from a text font sits on the text baseline
  rather than being centered on the line.

## Text fonts

- Rasterized with FreeType, light hinting (`FT_LOAD_TARGET_LIGHT`).
- `line_height`/`base_line` come from the face's ascender/descender, not from
  the included glyphs, so changing the `glyph` subset never moves layout.
  lv_font_conv does the opposite, so its fonts may be a few px tighter.
- Characters in `glyph` that the font lacks are skipped with a build warning.
- No kerning and no RLE compression yet.
- With `CONFIG_LV_FONT_FMT_TXT_LARGE` off, a font is limited to 1 MB of bitmap
  and 255 px glyph boxes; a generated font that exceeds this carries an
  `#error` asking for the option.

## Images

Pixels are stored in LVGL's native little-endian layout with a packed stride
(`LV_DRAW_BUF_STRIDE_ALIGN` is 1 here). Formats without alpha drop the source
alpha instead of compositing it.

## Build integration

- `resgen.py cmake` runs at configure time and lists each entry's input files;
  the JSON and the script are `CMAKE_CONFIGURE_DEPENDS`. `app/CMakeLists.txt`
  finds the JSON through a `CONFIGURE_DEPENDS` glob, so adding or removing it
  reconfigures without a manual `cmake`.
- Each font/image is its own custom command, so Ninja runs them in parallel and
  an SVG edit only regenerates the fonts that use it.
- Outputs are only written when their content changes. CMake's Ninja custom
  commands are `restat`, so a JSON edit reruns every entry but only recompiles
  the `.c` files whose content actually changed.
- The simulator's `idf_component_register` shim adds sources to the
  `simulator` target, which is defined in another directory. Custom command
  outputs are not attached to a target in a different directory, hence the
  `resgen_<target>` custom target plus `GENERATED` on the target's directory in
  `resgen.cmake`.
