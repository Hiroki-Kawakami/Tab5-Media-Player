#!/usr/bin/env python3
import argparse
import io
import json
import math
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ICON_CODEPOINT_START = 0xE000
ICON_CODEPOINT_END = 0xF000
HEADER_NAME = "resources.h"
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

IMAGE_FORMATS = {
    "RGB565": ("LV_COLOR_FORMAT_RGB565", 2),
    "RGB565A8": ("LV_COLOR_FORMAT_RGB565A8", 2),
    "RGB888": ("LV_COLOR_FORMAT_RGB888", 3),
    "XRGB8888": ("LV_COLOR_FORMAT_XRGB8888", 4),
    "ARGB8888": ("LV_COLOR_FORMAT_ARGB8888", 4),
    "L8": ("LV_COLOR_FORMAT_L8", 1),
    "A8": ("LV_COLOR_FORMAT_A8", 1),
}


class DefinitionError(Exception):
    pass


def fail(where, message):
    raise DefinitionError(f"{where}: {message}")


def check_keys(where, obj, allowed):
    if not isinstance(obj, dict):
        fail(where, "must be an object")
    unknown = set(obj) - set(allowed)
    if unknown:
        fail(where, f"unknown keys {sorted(unknown)}")


def check_ident(where, name):
    if not IDENT_RE.match(name):
        fail(where, f"'{name}' is not a valid C identifier")


def check_int(where, value, minimum=1):
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        fail(where, f"must be an integer >= {minimum}")


class Definition:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.base = self.path.parent
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            raise DefinitionError(f"{self.path}: {e}")
        check_keys(str(self.path), data, ("font", "image"))
        self.fonts = data.get("font", {})
        self.images = data.get("image", {})
        for key, value in (("font", self.fonts), ("image", self.images)):
            if not isinstance(value, dict):
                fail(key, "must be an object")
        for name in list(self.fonts) + list(self.images):
            check_ident(name, name)
        clash = set(self.fonts) & set(self.images)
        if clash:
            fail("definition", f"names used by both font and image: {sorted(clash)}")
        for name, font in self.fonts.items():
            self._validate_font(f"font.{name}", font)
        for name, image in self.images.items():
            self._validate_image(f"image.{name}", image)
        self.icon_codepoints = self._assign_icon_codepoints()

    def _validate_font(self, where, font):
        check_keys(where, font,
                   ("size", "bpp", "icon", "font", "glyph", "glyph_file", "variation", "fallback",
                    "pack", "compress"))
        check_int(f"{where}.size", font.get("size"))
        if font.get("bpp", 4) not in (1, 2, 4, 8):
            fail(f"{where}.bpp", "must be 1, 2, 4 or 8")
        if ("icon" in font) == ("font" in font):
            fail(where, "needs exactly one of 'icon' or 'font'")
        if "icon" in font:
            if ("glyph" in font or "glyph_file" in font or "variation" in font or "pack" in font
                    or "compress" in font):
                fail(where, "'glyph', 'glyph_file', 'variation', 'pack' and 'compress' only apply to 'font'")
            icons = font["icon"]
            if not isinstance(icons, dict) or not icons:
                fail(f"{where}.icon", "must be a non-empty object")
            for icon, file in icons.items():
                check_ident(f"{where}.icon", icon)
                if not isinstance(file, str):
                    fail(f"{where}.icon.{icon}", "must be a file name")
        else:
            if not isinstance(font["font"], str):
                fail(f"{where}.font", "must be a file name")
            glyph = font.get("glyph")
            if glyph is not None and (not isinstance(glyph, list)
                                      or not all(isinstance(s, str) for s in glyph)):
                fail(f"{where}.glyph", "must be an array of strings")
            if "glyph_file" in font and not isinstance(font["glyph_file"], str):
                fail(f"{where}.glyph_file", "must be a file name")
            if "pack" in font:
                if font["pack"] is not True:
                    fail(f"{where}.pack", "must be true")
                if "fallback" in font:
                    fail(f"{where}.fallback", "a pack is chained at runtime, not by resgen")
                if font.get("bpp", 4) == 8:
                    fail(f"{where}.bpp", "a pack stores 1, 2 or 4 bpp")
            if "compress" in font:
                if not isinstance(font["compress"], bool):
                    fail(f"{where}.compress", "must be true or false")
                if not font.get("pack"):
                    fail(f"{where}.compress", "only applies to a pack")
            variation = font.get("variation", {})
            if not isinstance(variation, dict) or not all(
                    isinstance(v, (int, float)) and not isinstance(v, bool)
                    for v in variation.values()):
                fail(f"{where}.variation", "must map axis tags to numbers")
        if "fallback" in font:
            if not isinstance(font["fallback"], str):
                fail(f"{where}.fallback", "must be a font name")
            check_ident(f"{where}.fallback", font["fallback"])
            if font["fallback"] in self.images:
                fail(f"{where}.fallback", f"'{font['fallback']}' is an image")

    def _validate_image(self, where, image):
        check_keys(where, image, ("file", "format", "width", "height"))
        if not isinstance(image.get("file"), str):
            fail(f"{where}.file", "is required")
        if image.get("format") not in IMAGE_FORMATS:
            fail(f"{where}.format", f"is required, one of {list(IMAGE_FORMATS)}")
        for key in ("width", "height"):
            if key in image:
                check_int(f"{where}.{key}", image[key])

    def _assign_icon_codepoints(self):
        codepoints = {}
        for font in self.fonts.values():
            for icon in font.get("icon", {}):
                if icon not in codepoints:
                    codepoints[icon] = ICON_CODEPOINT_START + len(codepoints)
        if ICON_CODEPOINT_START + len(codepoints) > ICON_CODEPOINT_END:
            fail("font", "too many icons")
        return codepoints

    def file(self, relative):
        return self.base / relative

    def inputs(self, name):
        if name in self.fonts:
            font = self.fonts[name]
            if "icon" in font:
                return sorted({self.file(f) for f in font["icon"].values()})
            files = [self.file(font["font"])]
            if "glyph_file" in font:
                files.append(self.file(font["glyph_file"]))
            return files
        return [self.file(self.images[name]["file"])]


def write_if_changed(path, text):
    path = Path(path)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def utf8_literal(codepoint):
    return "".join(f"\\x{b:02X}" for b in chr(codepoint).encode("utf-8"))


def hex_rows(data, per_row=16):
    return "\n".join(
        "    " + " ".join(f"0x{b:02x}," for b in data[i:i + per_row])
        for i in range(0, len(data), per_row))


PACK_TYPES = """typedef struct {
    uint32_t bitmap;
    uint16_t adv_w;
    uint8_t box_w;
    uint8_t box_h;
    int8_t ofs_x;
    int8_t ofs_y;
} resgen_glyph_t;

typedef struct {
    const uint16_t *codepoints;
    const resgen_glyph_t *glyphs;
    const uint8_t *data;
    uint32_t glyph_count;
    uint8_t px;
    uint8_t bpp;
    uint8_t prefilter;
    int16_t line_height;
    int16_t base_line;
    int16_t max_ascent;
    int16_t max_descent;
    int8_t underline_position;
    int8_t underline_thickness;
} resgen_font_pack_t;
"""


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.bits = 0

    def write(self, value, count):
        self.acc = (self.acc << count) | value
        self.bits += count
        while self.bits >= 8:
            self.bits -= 8
            self.out.append((self.acc >> self.bits) & 0xFF)
        self.acc &= (1 << self.bits) - 1

    def bytes(self):
        if self.bits == 0:
            return bytes(self.out)
        return bytes(self.out) + bytes(((self.acc << (8 - self.bits)) & 0xFF,))


# A value costs bpp bits. A value equal to the previous one switches to repeat
# mode, where every further repeat is a single 1 bit, a 0 bit ends the run and
# is followed by the next value, and the 11th repeat is followed by a 6 bit
# count. The count runs out on a value, not on a repeat, and the decoder does
# not re-enter repeat mode on that value.
def rle_encode(levels, bpp):
    writer = BitWriter()
    total = len(levels)
    index = 0
    previous = None
    repeats = 0
    repeating = False
    while index < total:
        if not repeating:
            value = levels[index]
            writer.write(value, bpp)
            index += 1
            repeating = previous is not None and value == previous
            repeats = 0
            previous = value
        elif index < total and levels[index] == previous:
            writer.write(1, 1)
            repeats += 1
            index += 1
            if repeats == 11:
                run = 0
                while index + run < total and levels[index + run] == previous:
                    run += 1
                length = min(run + 1, 63)
                writer.write(length, 6)
                index += length - 1
                if index < total:
                    writer.write(levels[index], bpp)
                    previous = levels[index]
                    index += 1
                repeating = False
        else:
            writer.write(0, 1)
            repeating = False
    return writer.bytes()


def rle_decode(data, count, bpp):
    bits = len(data) * 8
    position = 0

    def read(length):
        nonlocal position
        value = 0
        for i in range(length):
            bit = 0
            if position + i < bits:
                byte = data[(position + i) >> 3]
                bit = (byte >> (7 - ((position + i) & 7))) & 1
            value = (value << 1) | bit
        position += length
        return value

    out = []
    state = "single"
    previous = 0
    repeats = 0
    for _ in range(count):
        if state == "single":
            first = position == 0
            value = read(bpp)
            if not first and value == previous:
                state = "repeat"
                repeats = 0
            previous = value
        elif state == "repeat":
            repeats += 1
            if read(1) == 1:
                value = previous
                if repeats == 11:
                    repeats = read(6)
                    if repeats:
                        state = "count"
                    else:
                        value = read(bpp)
                        previous = value
                        state = "single"
            else:
                value = read(bpp)
                previous = value
                state = "single"
        else:
            value = previous
            repeats -= 1
            if repeats == 0:
                value = read(bpp)
                previous = value
                state = "single"
        out.append(value)
    return out


def unprefilter_rows(levels, width, height):
    out = []
    previous = [0] * width
    for y in range(height):
        row = [a ^ b for a, b in zip(levels[y * width:(y + 1) * width], previous)]
        out += row
        previous = row
    return out


def prefilter_rows(levels, width, height):
    out = []
    previous = [0] * width
    for y in range(height):
        row = levels[y * width:(y + 1) * width]
        out += [a ^ b for a, b in zip(row, previous)]
        previous = row
    return out


def generate_header(definition):
    lines = [
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    packs = [name for name, font in definition.fonts.items() if font.get("pack")]
    lines += [f"LV_FONT_DECLARE({name})" for name in definition.fonts if name not in packs]
    if packs:
        lines += ["", PACK_TYPES]
        lines += [f"extern const resgen_font_pack_t {name};" for name in packs]
    lines += [f"LV_IMAGE_DECLARE({name});" for name in definition.images]
    if definition.icon_codepoints:
        lines.append("")
        width = max(len(n) for n in definition.icon_codepoints)
        lines += [f'#define {name:<{width}} "{utf8_literal(cp)}"'
                  for name, cp in definition.icon_codepoints.items()]
    lines += [
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
    ]
    return "\n".join(lines)


class Glyph:
    def __init__(self, codepoint, adv_w, levels, width, height, ofs_x, ofs_y):
        self.codepoint = codepoint
        self.adv_w = adv_w
        self.levels = levels
        self.width = width
        self.height = height
        self.ofs_x = ofs_x
        self.ofs_y = ofs_y


def quantize(alpha, bpp):
    maxv = (1 << bpp) - 1
    return [(a * maxv + 127) // 255 for a in alpha]


def make_glyph(codepoint, adv_w, alpha, width, height, left, bottom, bpp):
    levels = quantize(alpha, bpp)
    rows = [levels[y * width:(y + 1) * width] for y in range(height)]
    ys = [y for y, row in enumerate(rows) if any(row)]
    if not ys:
        return Glyph(codepoint, adv_w, [], 0, 0, 0, 0)
    xs = [x for x in range(width) if any(rows[y][x] for y in ys)]
    x0, x1, y0, y1 = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
    trimmed = [v for y in range(y0, y1) for v in rows[y][x0:x1]]
    return Glyph(codepoint, adv_w, trimmed, x1 - x0, y1 - y0,
                 left + x0, bottom + (height - y1))


def pack_levels(levels, bpp):
    if bpp == 8:
        return bytes(levels)
    per_byte = 8 // bpp
    out = bytearray()
    for i in range(0, len(levels), per_byte):
        chunk = levels[i:i + per_byte]
        byte = 0
        for j in range(per_byte):
            byte = (byte << bpp) | (chunk[j] if j < len(chunk) else 0)
        out.append(byte)
    return bytes(out)


def build_cmaps(codepoints):
    runs = []
    for cp in codepoints:
        if runs and cp == runs[-1][-1] + 1:
            runs[-1].append(cp)
        else:
            runs.append([cp])
    cmaps = []
    for run in runs:
        if len(run) >= 8:
            cmaps.append(("range", run))
        elif cmaps and cmaps[-1][0] == "sparse" and run[-1] - cmaps[-1][1][0] <= 0xFFFF:
            cmaps[-1][1].extend(run)
        else:
            cmaps.append(("sparse", list(run)))
    if len(cmaps) > 511:
        raise DefinitionError("too many character map ranges")
    return cmaps


def generate_font_c(name, font, glyphs, line_height, base_line,
                    underline_position=0, underline_thickness=0):
    bpp = font.get("bpp", 4)
    glyphs = sorted(glyphs, key=lambda g: g.codepoint)
    bitmap = bytearray()
    dsc_rows = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"]
    needs_large = False
    for g in glyphs:
        index = len(bitmap)
        bitmap += pack_levels(g.levels, bpp)
        if (index >= 1 << 20 or g.adv_w >= 1 << 12 or g.width > 255 or g.height > 255
                or not -128 <= g.ofs_x <= 127 or not -128 <= g.ofs_y <= 127):
            needs_large = True
        dsc_rows.append(
            f"    {{.bitmap_index = {index}, .adv_w = {g.adv_w}, .box_w = {g.width}, "
            f".box_h = {g.height}, .ofs_x = {g.ofs_x}, .ofs_y = {g.ofs_y}}}, /* U+{g.codepoint:04X} */")

    cmaps = build_cmaps([g.codepoint for g in glyphs])
    lists = []
    cmap_rows = []
    glyph_id = 1
    for i, (kind, cps) in enumerate(cmaps):
        start = cps[0]
        length = cps[-1] - start + 1
        if kind == "range":
            cmap_rows.append(
                f"    {{.range_start = {start}, .range_length = {length}, .glyph_id_start = {glyph_id}, "
                f".unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
                f".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}},")
        else:
            offsets = ", ".join(str(cp - start) for cp in cps)
            lists.append(f"static const uint16_t unicode_list_{i}[] = {{{offsets}}};")
            cmap_rows.append(
                f"    {{.range_start = {start}, .range_length = {length}, .glyph_id_start = {glyph_id}, "
                f".unicode_list = unicode_list_{i}, .glyph_id_ofs_list = NULL, .list_length = {len(cps)}, "
                f".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}},")
        glyph_id += len(cps)

    fallback = font.get("fallback")
    out = ['#include "lvgl.h"', ""]
    if needs_large:
        out += ["#if !LV_FONT_FMT_TXT_LARGE",
                f'#error "{name} needs CONFIG_LV_FONT_FMT_TXT_LARGE"',
                "#endif", ""]
    if fallback:
        out += [f"LV_FONT_DECLARE({fallback})", ""]
    out += [
        "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {",
        hex_rows(bitmap) if bitmap else "    0x00,",
        "};",
        "",
        "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {",
        *dsc_rows,
        "};",
        "",
        *lists,
        "",
        "static const lv_font_fmt_txt_cmap_t cmaps[] = {",
        *cmap_rows,
        "};",
        "",
        "static const lv_font_fmt_txt_dsc_t font_dsc = {",
        "    .glyph_bitmap = glyph_bitmap,",
        "    .glyph_dsc = glyph_dsc,",
        "    .cmaps = cmaps,",
        "    .kern_dsc = NULL,",
        "    .kern_scale = 0,",
        f"    .cmap_num = {len(cmaps)},",
        f"    .bpp = {bpp},",
        "    .kern_classes = 0,",
        "    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,",
        "};",
        "",
        f"const lv_font_t {name} = {{",
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
        f"    .line_height = {line_height},",
        f"    .base_line = {base_line},",
        "    .subpx = LV_FONT_SUBPX_NONE,",
        f"    .underline_position = {underline_position},",
        f"    .underline_thickness = {underline_thickness},",
        "    .dsc = &font_dsc,",
        f"    .fallback = {'&' + fallback if fallback else 'NULL'},",
        "};",
        "",
    ]
    return "\n".join(out)


def svg_aspect(path):
    root = ET.parse(path).getroot()
    view_box = root.get("viewBox")
    if view_box:
        _, _, w, h = (float(v) for v in re.split(r"[\s,]+", view_box.strip()))
    else:
        w = float(re.sub(r"[a-z%]+$", "", root.get("width", "0")))
        h = float(re.sub(r"[a-z%]+$", "", root.get("height", "0")))
    if w <= 0 or h <= 0:
        raise DefinitionError(f"{path}: cannot determine size (no viewBox)")
    return w / h


def render_svg_alpha(path, width, height):
    import resvg_py
    from PIL import Image

    png = resvg_py.svg_to_bytes(svg_path=str(path), width=width, height=height)
    image = Image.open(io.BytesIO(bytes(png))).convert("RGBA")
    if image.size != (width, height):
        image = image.resize((width, height), Image.Resampling.LANCZOS)
    return list(image.getchannel("A").tobytes())


def generate_icon_font(definition, name):
    font = definition.fonts[name]
    size = font["size"]
    bpp = font.get("bpp", 4)
    glyphs = []
    for icon, file in font["icon"].items():
        path = definition.file(file)
        if not path.exists():
            raise DefinitionError(f"font.{name}.icon.{icon}: {path} not found")
        width = max(1, round(size * svg_aspect(path)))
        alpha = render_svg_alpha(path, width, size)
        glyphs.append(make_glyph(definition.icon_codepoints[icon], width * 16,
                                 alpha, width, size, 0, 0, bpp))
    return generate_font_c(name, font, glyphs, line_height=size, base_line=0)


def glyph_codepoints(definition, where, face, font):
    chars = {c for s in font.get("glyph", []) for c in s}
    if "glyph_file" in font:
        path = definition.file(font["glyph_file"])
        if not path.exists():
            raise DefinitionError(f"{where}.glyph_file: {path} not found")
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.startswith("#"):
                chars |= set(line)
    if chars:
        return sorted({ord(c) for c in chars if c not in "\r\n\t"})
    return sorted(cp for cp, _ in face.get_chars()
                  if cp >= 0x20 and not 0x7F <= cp <= 0x9F)


def set_variation(face, where, variation):
    if not variation:
        return
    if not face.has_multiple_masters:
        raise DefinitionError(f"{where}.variation: font has no variation axes")
    axes = face.get_variation_info().axes
    tags = [a.tag for a in axes]
    unknown = set(variation) - set(tags)
    if unknown:
        raise DefinitionError(f"{where}.variation: unknown axes {sorted(unknown)}, font has {tags}")
    # freetype-py reports the axis limits in 16.16 but takes design coordinates
    # as plain numbers.
    face.set_var_design_coords([variation.get(a.tag, a.default / 65536.0) for a in axes])


def render_ttf_glyphs(definition, name):
    import freetype

    where = f"font.{name}"
    font = definition.fonts[name]
    size = font["size"]
    bpp = font.get("bpp", 4)
    path = definition.file(font["font"])
    if not path.exists():
        raise DefinitionError(f"{where}.font: {path} not found")
    face = freetype.Face(str(path))
    set_variation(face, where, font.get("variation"))
    face.set_pixel_sizes(0, size)

    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT
    glyphs = []
    missing = []
    for cp in glyph_codepoints(definition, where, face, font):
        if face.get_char_index(cp) == 0:
            missing.append(cp)
            continue
        face.load_char(cp, flags)
        slot = face.glyph
        bm = slot.bitmap
        alpha = []
        for y in range(bm.rows):
            alpha += bm.buffer[y * bm.pitch:y * bm.pitch + bm.width]
        adv_w = round(slot.advance.x / 4)
        glyphs.append(make_glyph(cp, adv_w, alpha, bm.width, bm.rows,
                                 slot.bitmap_left, slot.bitmap_top - bm.rows, bpp))
    if missing:
        shown = " ".join(f"U+{cp:04X}" for cp in missing[:20])
        more = f" (+{len(missing) - 20})" if len(missing) > 20 else ""
        print(f"resgen: warning: {where}: {len(missing)} glyphs not in {path.name}: {shown}{more}",
              file=sys.stderr)

    metrics = face.size
    ascender = math.ceil(metrics.ascender / 64)
    descender = math.floor(metrics.descender / 64)
    scale = size / face.units_per_EM
    return glyphs, {
        "line_height": ascender - descender,
        "base_line": -descender,
        "underline_position": round(face.underline_position * scale),
        "underline_thickness": max(1, round(face.underline_thickness * scale)),
    }


def generate_ttf_font(definition, name):
    glyphs, metrics = render_ttf_glyphs(definition, name)
    return generate_font_c(name, definition.fonts[name], glyphs, **metrics)


def encode_pack_glyph(glyph, bpp, compress, prefilter):
    raw = pack_levels(glyph.levels, bpp)
    if not compress:
        return raw, False
    source = prefilter_rows(glyph.levels, glyph.width, glyph.height) if prefilter else glyph.levels
    rle = rle_encode(source, bpp)
    if len(rle) < len(raw):
        return rle, True
    return raw, False


def generate_pack_c(name, font, glyphs, metrics, check=False):
    bpp = font.get("bpp", 4)
    prefilter = font.get("compress", True)
    glyphs = sorted(glyphs, key=lambda g: g.codepoint)
    if glyphs and glyphs[-1].codepoint > 0xFFFF:
        raise DefinitionError(f"font.{name}: a pack stores codepoints up to U+FFFF")

    data = bytearray()
    rows = []
    for g in glyphs:
        if not g.levels:
            rows.append((0, g))
            continue
        encoded, compressed = encode_pack_glyph(g, bpp, font.get("compress", True), prefilter)
        if check:
            levels = rle_decode(encoded, len(g.levels), bpp) if compressed else None
            if compressed:
                if prefilter:
                    levels = unprefilter_rows(levels, g.width, g.height)
                if levels != g.levels:
                    raise DefinitionError(f"font.{name}: U+{g.codepoint:04X} does not decode back")
        if g.width > 255 or g.height > 255:
            raise DefinitionError(f"font.{name}: U+{g.codepoint:04X} is larger than 255 px")
        rows.append((len(data) | (0x80000000 if compressed else 0), g))
        data += encoded

    # the decoder reads a 24 bit window, so the last glyph must have slack
    data += b"\x00\x00"

    glyph_rows = [
        f"    {{.bitmap = 0x{bitmap:08x}, .adv_w = {g.adv_w}, .box_w = {g.width}, "
        f".box_h = {g.height}, .ofs_x = {g.ofs_x}, .ofs_y = {g.ofs_y}}}, /* U+{g.codepoint:04X} */"
        for bitmap, g in rows]
    codepoints = [f"0x{g.codepoint:04x}," for g in glyphs]
    return "\n".join([
        '#include "resources.h"',
        "",
        "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_data[] = {",
        hex_rows(data) if data else "    0x00,",
        "};",
        "",
        "static const uint16_t codepoints[] = {",
        *["    " + " ".join(codepoints[i:i + 12]) for i in range(0, len(codepoints), 12)],
        "};",
        "",
        "static const resgen_glyph_t glyphs[] = {",
        *glyph_rows,
        "};",
        "",
        f"const resgen_font_pack_t {name} = {{",
        "    .codepoints = codepoints,",
        "    .glyphs = glyphs,",
        "    .data = glyph_data,",
        f"    .glyph_count = {len(glyphs)},",
        f"    .px = {font['size']},",
        f"    .bpp = {bpp},",
        f"    .prefilter = {1 if prefilter else 0},",
        f"    .line_height = {metrics['line_height']},",
        f"    .base_line = {metrics['base_line']},",
        f"    .max_ascent = {max((g.height + g.ofs_y for g in glyphs), default=0)},",
        f"    .max_descent = {max((-g.ofs_y for g in glyphs), default=0)},",
        f"    .underline_position = {metrics['underline_position']},",
        f"    .underline_thickness = {metrics['underline_thickness']},",
        "};",
        "",
    ])


def generate_pack_font(definition, name, check=False):
    glyphs, metrics = render_ttf_glyphs(definition, name)
    return generate_pack_c(name, definition.fonts[name], glyphs, metrics, check)


def encode_image(image, fmt):
    from PIL import Image

    w, h = image.size
    has_alpha = image.mode in ("RGBA", "LA", "PA") or (image.mode == "P" and "transparency" in image.info)
    if fmt == "A8":
        if not has_alpha:
            raise DefinitionError("A8 needs a source image with an alpha channel")
        return image.convert("RGBA").getchannel("A").tobytes()
    if fmt == "L8":
        return image.convert("L").tobytes()
    rgba = image.convert("RGBA").tobytes()
    px = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if fmt in ("RGB565", "RGB565A8"):
        out = bytearray()
        for r, g, b, _ in px:
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += bytes((v & 0xFF, v >> 8))
        if fmt == "RGB565A8":
            out += bytes(p[3] for p in px)
        return bytes(out)
    if fmt == "RGB888":
        return bytes(c for r, g, b, _ in px for c in (b, g, r))
    if fmt == "XRGB8888":
        return bytes(c for r, g, b, _ in px for c in (b, g, r, 0xFF))
    if fmt == "ARGB8888":
        return bytes(c for r, g, b, a in px for c in (b, g, r, a))
    raise AssertionError(fmt)


def generate_image(definition, name):
    from PIL import Image, ImageOps

    where = f"image.{name}"
    spec = definition.images[name]
    path = definition.file(spec["file"])
    if not path.exists():
        raise DefinitionError(f"{where}.file: {path} not found")
    image = ImageOps.exif_transpose(Image.open(path))
    w, h = image.size
    if "width" in spec and "height" in spec:
        size = (spec["width"], spec["height"])
    elif "width" in spec:
        size = (spec["width"], max(1, round(h * spec["width"] / w)))
    elif "height" in spec:
        size = (max(1, round(w * spec["height"] / h)), spec["height"])
    else:
        size = (w, h)
    if size != (w, h):
        image = image.convert("RGBA").resize(size, Image.Resampling.LANCZOS)
    fmt = spec["format"]
    try:
        data = encode_image(image, fmt)
    except DefinitionError as e:
        raise DefinitionError(f"{where}: {e}")
    cf, bytes_per_px = IMAGE_FORMATS[fmt]
    w, h = size
    return "\n".join([
        '#include "lvgl.h"',
        "",
        "static LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST const uint8_t image_data[] = {",
        hex_rows(data),
        "};",
        "",
        f"const lv_image_dsc_t {name} = {{",
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        f"        .cf = {cf},",
        f"        .w = {w},",
        f"        .h = {h},",
        f"        .stride = {w * bytes_per_px},",
        "    },",
        f"    .data_size = {len(data)},",
        "    .data = image_data,",
        "};",
        "",
    ])


def cmake_quote(value):
    return '"' + str(value).replace("\\", "/").replace('"', '\\"') + '"'


def generate_cmake(definition):
    names = list(definition.fonts) + list(definition.images)
    lines = [f"set(RESGEN_ENTRIES {' '.join(names)})"]
    for name in names:
        for path in definition.inputs(name):
            if not path.exists():
                raise DefinitionError(f"{name}: {path} not found")
        inputs = " ".join(cmake_quote(p) for p in definition.inputs(name))
        lines.append(f"set(RESGEN_INPUTS_{name} {inputs})")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Generate LVGL v9 fonts and images from a JSON definition")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("cmake", help="write the entry/input list for CMake")
    p.add_argument("definition")
    p.add_argument("output")
    p = sub.add_parser("header", help=f"write {HEADER_NAME}")
    p.add_argument("definition")
    p.add_argument("outdir")
    p = sub.add_parser("entry", help="write <name>.c for one font or image")
    p.add_argument("definition")
    p.add_argument("name")
    p.add_argument("outdir")
    p = sub.add_parser("all", help="write the header and every entry")
    p.add_argument("definition")
    p.add_argument("outdir")
    p = sub.add_parser("check", help="verify that every packed glyph decodes back")
    p.add_argument("definition")
    args = parser.parse_args()

    try:
        definition = Definition(args.definition)
        if args.command == "cmake":
            write_if_changed(args.output, generate_cmake(definition))
            return
        if args.command == "check":
            for name, font in definition.fonts.items():
                if font.get("pack"):
                    generate_pack_font(definition, name, check=True)
                    print(f"resgen: {name}: every glyph decodes back")
            return
        outdir = Path(args.outdir)
        if args.command in ("header", "all"):
            write_if_changed(outdir / HEADER_NAME, generate_header(definition))
        if args.command == "entry":
            names = [args.name]
        elif args.command == "all":
            names = list(definition.fonts) + list(definition.images)
        else:
            names = []
        for name in names:
            if name in definition.fonts:
                if "icon" in definition.fonts[name]:
                    text = generate_icon_font(definition, name)
                elif definition.fonts[name].get("pack"):
                    text = generate_pack_font(definition, name)
                else:
                    text = generate_ttf_font(definition, name)
            elif name in definition.images:
                text = generate_image(definition, name)
            else:
                raise DefinitionError(f"no font or image named '{name}'")
            write_if_changed(outdir / f"{name}.c", text)
    except DefinitionError as e:
        print(f"resgen: error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
