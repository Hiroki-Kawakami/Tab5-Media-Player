/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_tags.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

typedef struct {
    char *out;
    size_t capacity;
    size_t length;
} writer_t;

static char *field_of(media_tags_t *tags, media_tag_field_t field, size_t *capacity) {
    switch (field) {
    case MEDIA_TAG_TITLE: *capacity = sizeof(tags->title); return tags->title;
    case MEDIA_TAG_ARTIST: *capacity = sizeof(tags->artist); return tags->artist;
    case MEDIA_TAG_ALBUM: *capacity = sizeof(tags->album); return tags->album;
    case MEDIA_TAG_ALBUM_ARTIST: *capacity = sizeof(tags->album_artist); return tags->album_artist;
    case MEDIA_TAG_TRACK: *capacity = sizeof(tags->track); return tags->track;
    case MEDIA_TAG_DATE: *capacity = sizeof(tags->date); return tags->date;
    default: *capacity = 0; return NULL;
    }
}

static bool put(writer_t *writer, uint32_t code) {
    if (code < 0x20 || code == 0x7F) code = ' ';
    const size_t bytes = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    if (writer->length + bytes + 1 > writer->capacity) return false;

    char *out = writer->out + writer->length;
    switch (bytes) {
    case 1:
        out[0] = (char)code;
        break;
    case 2:
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        break;
    case 3:
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        break;
    default:
        out[0] = (char)(0xF0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code & 0x3F));
        break;
    }
    writer->length += bytes;
    return true;
}

static void convert_latin1(writer_t *writer, const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size && bytes[i]; i++) {
        if (!put(writer, bytes[i])) return;
    }
}

static void convert_utf16(writer_t *writer, const uint8_t *bytes, size_t size, bool big_endian) {
    if (size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        big_endian = false;
        bytes += 2;
        size -= 2;
    } else if (size >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        big_endian = true;
        bytes += 2;
        size -= 2;
    }

    for (size_t i = 0; i + 2 <= size; i += 2) {
        uint32_t code = big_endian ? (uint32_t)bytes[i] << 8 | bytes[i + 1]
                                   : (uint32_t)bytes[i + 1] << 8 | bytes[i];
        if (!code) return;
        if (code >= 0xD800 && code < 0xDC00) {
            if (i + 4 > size) return;
            const uint32_t low = big_endian ? (uint32_t)bytes[i + 2] << 8 | bytes[i + 3]
                                            : (uint32_t)bytes[i + 3] << 8 | bytes[i + 2];
            if (low < 0xDC00 || low > 0xDFFF) return;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            i += 2;
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return;
        }
        if (!put(writer, code)) return;
    }
}

static void convert_utf8(writer_t *writer, const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size && bytes[i];) {
        const uint8_t lead = bytes[i];
        size_t length;
        uint32_t code;
        if (lead < 0x80) {
            length = 1;
            code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07;
        } else {
            return;
        }
        if (i + length > size) return;
        for (size_t k = 1; k < length; k++) {
            if ((bytes[i + k] & 0xC0) != 0x80) return;
            code = code << 6 | (bytes[i + k] & 0x3F);
        }
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return;
        if (length > 1 && code < (uint32_t)(length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000)) {
            return;
        }
        if (!put(writer, code)) return;
        i += length;
    }
}

const char *media_tags_get(const media_tags_t *tags, media_tag_field_t field) {
    size_t capacity = 0;
    const char *text = field_of((media_tags_t *)tags, field, &capacity);
    return text ? text : "";
}

bool media_tags_empty(const media_tags_t *tags) {
    for (int field = 0; field < MEDIA_TAG_COUNT; field++) {
        if (media_tags_get(tags, field)[0]) return false;
    }
    return true;
}

void media_tags_set(media_tags_t *tags, media_tag_field_t field, const void *bytes, size_t size,
                    media_text_encoding_t encoding) {
    size_t capacity = 0;
    char *out = field_of(tags, field, &capacity);
    if (!out || out[0] || !bytes || !size) return;

    writer_t writer = { out, capacity, 0 };
    switch (encoding) {
    case MEDIA_TEXT_UTF16: convert_utf16(&writer, bytes, size, false); break;
    case MEDIA_TEXT_UTF16BE: convert_utf16(&writer, bytes, size, true); break;
    case MEDIA_TEXT_UTF8: convert_utf8(&writer, bytes, size); break;
    default: convert_latin1(&writer, bytes, size); break;
    }

    while (writer.length && out[writer.length - 1] == ' ') writer.length--;
    size_t start = 0;
    while (start < writer.length && out[start] == ' ') start++;
    if (start) memmove(out, out + start, writer.length - start);
    out[writer.length - start] = '\0';
}

void media_tags_set_number(media_tags_t *tags, media_tag_field_t field, uint32_t value,
                           uint32_t total) {
    size_t capacity = 0;
    char *out = field_of(tags, field, &capacity);
    if (!out || out[0] || !value) return;
    if (total) {
        snprintf(out, capacity, "%u/%u", (unsigned)value, (unsigned)total);
    } else {
        snprintf(out, capacity, "%u", (unsigned)value);
    }
}

static media_cover_format_t cover_format(const uint8_t *data, size_t size) {
    static const uint8_t kPng[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return MEDIA_COVER_JPEG;
    }
    if (size >= sizeof(kPng) && memcmp(data, kPng, sizeof(kPng)) == 0) return MEDIA_COVER_PNG;
    return MEDIA_COVER_NONE;
}

void media_tags_set_cover(media_tags_t *tags, const void *data, size_t size, bool front) {
    if (!data || size > MEDIA_COVER_MAX_BYTES) return;
    const media_cover_format_t format = cover_format(data, size);
    if (format == MEDIA_COVER_NONE) return;
    if (tags->cover.data && (!front || tags->cover.front)) return;

    uint8_t *copy = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!copy) copy = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    if (!copy) return;
    memcpy(copy, data, size);

    heap_caps_free(tags->cover.data);
    tags->cover.data = copy;
    tags->cover.size = (uint32_t)size;
    tags->cover.format = format;
    tags->cover.front = front;
}

void media_tags_free(media_tags_t *tags) {
    heap_caps_free(tags->cover.data);
    memset(tags, 0, sizeof(*tags));
}
