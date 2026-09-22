/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "image_exif.hpp"
#include "image_codec.hpp"

#include <cstring>

namespace {

constexpr uint32_t kMaxEntries = 512;
/* A text tag is longer than any field here only in a file that means nothing
   good by it, and scanning all of one costs the probe real time. */
constexpr std::size_t kMaxAscii = 256;

struct Tiff {
    const uint8_t *data = nullptr;
    std::size_t size = 0;
    bool big_endian = false;

    bool has(std::size_t at, std::size_t bytes) const {
        return at <= size && size - at >= bytes;
    }
    uint16_t u16(std::size_t at) const {
        if (!has(at, 2)) return 0;
        return big_endian ? (uint16_t)((data[at] << 8) | data[at + 1])
                          : (uint16_t)((data[at + 1] << 8) | data[at]);
    }
    uint32_t u32(std::size_t at) const {
        if (!has(at, 4)) return 0;
        if (big_endian) {
            return ((uint32_t)data[at] << 24) | ((uint32_t)data[at + 1] << 16) |
                   ((uint32_t)data[at + 2] << 8) | data[at + 3];
        }
        return ((uint32_t)data[at + 3] << 24) | ((uint32_t)data[at + 2] << 16) |
               ((uint32_t)data[at + 1] << 8) | data[at];
    }
};

struct Entry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    std::size_t at = 0;
    std::size_t bytes = 0;
};

std::size_t type_bytes(uint16_t type) {
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

bool entry_at(const Tiff &tiff, std::size_t at, Entry *out) {
    if (!tiff.has(at, 12)) return false;
    out->tag = tiff.u16(at);
    out->type = tiff.u16(at + 2);
    out->count = tiff.u32(at + 4);
    const std::size_t unit = type_bytes(out->type);
    if (!unit || !out->count || out->count > tiff.size) return false;
    out->bytes = unit * (std::size_t)out->count;
    out->at = out->bytes <= 4 ? at + 8 : tiff.u32(at + 8);
    return tiff.has(out->at, out->bytes);
}

bool read_uint(const Tiff &tiff, const Entry &entry, uint32_t *out) {
    switch (entry.type) {
    case 1: case 6: *out = tiff.data[entry.at]; return true;
    case 3: case 8: *out = tiff.u16(entry.at); return true;
    case 4: case 9: *out = tiff.u32(entry.at); return true;
    default: return false;
    }
}

bool read_ratio(const Tiff &tiff, const Entry &entry, uint32_t *num, uint32_t *den) {
    if (entry.type != 5 && entry.type != 10) return false;
    *num = tiff.u32(entry.at);
    *den = tiff.u32(entry.at + 4);
    return *den != 0;
}

bool read_float(const Tiff &tiff, const Entry &entry, float *out) {
    uint32_t num, den;
    if (read_ratio(tiff, entry, &num, &den)) {
        *out = entry.type == 10 ? (float)((double)(int32_t)num / (int32_t)den)
                                : (float)((double)num / den);
        return true;
    }
    uint32_t value;
    if (!read_uint(tiff, entry, &value)) return false;
    *out = (float)value;
    return true;
}

/* The panel's font is ASCII, and a tag may carry anything: the bytes it cannot
   draw are dropped rather than handed to LVGL. */
void read_ascii(const Tiff &tiff, const Entry &entry, char *out, std::size_t cap) {
    if (entry.type != 2) return;
    const std::size_t limit = entry.bytes < kMaxAscii ? entry.bytes : kMaxAscii;
    std::size_t n = 0;
    for (std::size_t i = 0; i < limit && n + 1 < cap; i++) {
        const char c = (char)tiff.data[entry.at + i];
        if (!c) break;
        if (c < 0x20 || c > 0x7e) continue;
        out[n++] = c;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

/* IFD1 describes the thumbnail and nothing else: what it is compressed with,
   where it starts and how long it is, all relative to the TIFF header. */
void parse_thumb_ifd(const Tiff &tiff, std::size_t at, std::size_t base, ImageExif *out) {
    const uint32_t count = tiff.u16(at);
    uint32_t compression = 0;
    uint32_t offset = 0;
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < count && i < kMaxEntries; i++) {
        Entry entry;
        if (!entry_at(tiff, at + 2 + (std::size_t)i * 12, &entry)) continue;
        switch (entry.tag) {
        case 0x0103: read_uint(tiff, entry, &compression); break;
        case 0x0201: read_uint(tiff, entry, &offset); break;
        case 0x0202: read_uint(tiff, entry, &bytes); break;
        default: break;
        }
    }
    if (compression != 6 || !offset || !bytes) return;
    if (!tiff.has(offset, bytes)) return;
    if (tiff.data[offset] != 0xFF || tiff.data[offset + 1] != 0xD8) return;
    out->thumb_at = (uint32_t)(base + offset);
    out->thumb_bytes = bytes;
}

std::size_t parse_ifd(const Tiff &tiff, std::size_t at, ImageExif *out) {
    const uint32_t count = tiff.u16(at);
    std::size_t sub = 0;
    for (uint32_t i = 0; i < count && i < kMaxEntries; i++) {
        Entry entry;
        if (!entry_at(tiff, at + 2 + (std::size_t)i * 12, &entry)) continue;
        uint32_t value = 0;
        switch (entry.tag) {
        case 0x010F: read_ascii(tiff, entry, out->make, sizeof out->make); break;
        case 0x0110: read_ascii(tiff, entry, out->model, sizeof out->model); break;
        case 0x0131: read_ascii(tiff, entry, out->software, sizeof out->software); break;
        case 0xA434: read_ascii(tiff, entry, out->lens, sizeof out->lens); break;
        case 0x9003: read_ascii(tiff, entry, out->taken, sizeof out->taken); break;
        case 0x0132:
            if (!out->taken[0]) read_ascii(tiff, entry, out->taken, sizeof out->taken);
            break;
        case 0x0112:
            if (read_uint(tiff, entry, &value) && value >= 1 && value <= 8) {
                out->orientation = (uint8_t)value;
            }
            break;
        case 0x829A: read_ratio(tiff, entry, &out->shutter_num, &out->shutter_den); break;
        case 0x829D: read_float(tiff, entry, &out->aperture); break;
        case 0x920A: read_float(tiff, entry, &out->focal_mm); break;
        case 0x8827: case 0x8833:
            if (!out->iso && read_uint(tiff, entry, &value)) out->iso = value;
            break;
        case 0xA405:
            if (read_uint(tiff, entry, &value)) out->focal35_mm = (uint16_t)value;
            break;
        case 0x9204: out->has_bias = read_float(tiff, entry, &out->exposure_bias); break;
        case 0x9209:
            if (read_uint(tiff, entry, &value)) {
                out->flash = (uint16_t)value;
                out->has_flash = true;
            }
            break;
        case 0x8769:
            if (read_uint(tiff, entry, &value)) sub = value;
            break;
        default: break;
        }
    }
    return sub;
}

bool parse_tiff(const uint8_t *data, std::size_t size, std::size_t base, ImageExif *out) {
    Tiff tiff;
    tiff.data = data;
    tiff.size = size;
    if (size < 8) return false;
    if (memcmp(data, "II", 2) == 0) {
        tiff.big_endian = false;
    } else if (memcmp(data, "MM", 2) == 0) {
        tiff.big_endian = true;
    } else {
        return false;
    }
    if (tiff.u16(2) != 42) return false;

    const std::size_t ifd0 = tiff.u32(4);
    if (!tiff.has(ifd0, 2)) return false;
    const std::size_t sub = parse_ifd(tiff, ifd0, out);
    if (sub && sub != ifd0 && tiff.has(sub, 2)) parse_ifd(tiff, sub, out);
    const std::size_t next = tiff.u32(ifd0 + 2 + (std::size_t)tiff.u16(ifd0) * 12);
    if (next && next != ifd0 && tiff.has(next, 2)) parse_thumb_ifd(tiff, next, base, out);
    return !out->empty();
}

bool jpeg_exif(const uint8_t *data, std::size_t size, const uint8_t **out,
               std::size_t *out_size) {
    std::size_t i = 2;
    while (i + 4 <= size) {
        if (data[i] != 0xFF) return false;
        const uint8_t marker = data[i + 1];
        if (marker == 0xFF) {
            i++;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            i += 2;
            continue;
        }
        if (marker == 0xDA) return false;
        const std::size_t segment = ((std::size_t)data[i + 2] << 8) | data[i + 3];
        if (marker == 0xE1 && segment >= 8 && i + 10 <= size &&
            memcmp(data + i + 4, "Exif\0\0", 6) == 0) {
            const std::size_t from = i + 10;
            std::size_t bytes = segment - 8;
            if (bytes > size - from) bytes = size - from;
            *out = data + from;
            *out_size = bytes;
            return true;
        }
        i += 2 + segment;
    }
    return false;
}

bool png_exif(const uint8_t *data, std::size_t size, const uint8_t **out, std::size_t *out_size) {
    std::size_t i = 8;
    while (i + 8 <= size) {
        const std::size_t length = ((std::size_t)data[i] << 24) | ((std::size_t)data[i + 1] << 16) |
                                   ((std::size_t)data[i + 2] << 8) | data[i + 3];
        const uint8_t *type = data + i + 4;
        if (memcmp(type, "IDAT", 4) == 0) return false;
        if (memcmp(type, "eXIf", 4) == 0) {
            const std::size_t from = i + 8;
            std::size_t bytes = length;
            if (bytes > size - from) bytes = size - from;
            *out = data + from;
            *out_size = bytes;
            return true;
        }
        if (length > size) return false;
        i += 12 + length;
    }
    return false;
}

}

bool ImageExif::empty() const {
    return !make[0] && !model[0] && !lens[0] && !software[0] && !taken[0] && !iso &&
           !shutter_den && aperture == 0 && focal_mm == 0 && !focal35_mm && !orientation &&
           !has_flash && !has_bias && !thumb_bytes;
}

bool image_exif_parse(const uint8_t *data, std::size_t size, ImageExif *out) {
    *out = {};
    const uint8_t *tiff = nullptr;
    std::size_t bytes = 0;
    if (size >= 4 && data[0] == 0xFF && data[1] == 0xD8) {
        if (!jpeg_exif(data, size, &tiff, &bytes)) return false;
    } else if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        if (!png_exif(data, size, &tiff, &bytes)) return false;
    } else {
        return false;
    }
    if (!parse_tiff(tiff, bytes, (std::size_t)(tiff - data), out)) return false;

    /* The size decides whether the thumbnail can stand in for the picture at a
       given box, so a thumbnail whose own header will not parse is dropped. */
    if (out->thumb_bytes) {
        ImageHeader header;
        if (image_header(data + out->thumb_at, out->thumb_bytes, &header) &&
            header.format == ImageFormat::Jpeg) {
            out->thumb_width = (uint16_t)header.width;
            out->thumb_height = (uint16_t)header.height;
        } else {
            out->thumb_at = 0;
            out->thumb_bytes = 0;
        }
    }
    return true;
}
