/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "media_tags.h"

#define ID3_MAX_TAG_BYTES (4 * 1024 * 1024)
#define ID3_HEADER_BYTES 10
#define ID3V1_BYTES 128
/* Enough for any text frame worth keeping (media_tags_set truncates to 128
   bytes anyway) and for the mime/description prefix of a picture frame. */
#define ID3_SCRATCH_BYTES 2048

typedef struct {
    const char *id;
    media_tag_field_t field;
} frame_map_t;

static const frame_map_t kFrames3[] = {
    { "TIT2", MEDIA_TAG_TITLE },  { "TPE1", MEDIA_TAG_ARTIST },
    { "TALB", MEDIA_TAG_ALBUM },  { "TPE2", MEDIA_TAG_ALBUM_ARTIST },
    { "TRCK", MEDIA_TAG_TRACK },  { "TDRC", MEDIA_TAG_DATE },
    { "TYER", MEDIA_TAG_DATE },
};

static const frame_map_t kFrames2[] = {
    { "TT2", MEDIA_TAG_TITLE }, { "TP1", MEDIA_TAG_ARTIST }, { "TAL", MEDIA_TAG_ALBUM },
    { "TP2", MEDIA_TAG_ALBUM_ARTIST }, { "TRK", MEDIA_TAG_TRACK }, { "TYE", MEDIA_TAG_DATE },
};

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t synchsafe32(const uint8_t *p) {
    return (uint32_t)p[0] << 21 | (uint32_t)p[1] << 14 | (uint32_t)p[2] << 7 | p[3];
}

static size_t unsynchronise(uint8_t *data, size_t size) {
    size_t out = 0;
    for (size_t i = 0; i < size; i++) {
        data[out++] = data[i];
        if (data[i] == 0xFF && i + 1 < size && data[i + 1] == 0x00) i++;
    }
    return out;
}

static bool text_encoding(uint8_t code, media_text_encoding_t *encoding) {
    switch (code) {
    case 0: *encoding = MEDIA_TEXT_LATIN1; return true;
    case 1: *encoding = MEDIA_TEXT_UTF16; return true;
    case 2: *encoding = MEDIA_TEXT_UTF16BE; return true;
    case 3: *encoding = MEDIA_TEXT_UTF8; return true;
    default: return false;
    }
}

static const frame_map_t *lookup(const char *id, const frame_map_t *frames, size_t count,
                                 size_t id_bytes) {
    for (size_t i = 0; i < count; i++) {
        if (memcmp(id, frames[i].id, id_bytes) == 0) return &frames[i];
    }
    return NULL;
}

/* Returns the offset just past the terminator of the string at `data`. */
static size_t skip_string(const uint8_t *data, size_t size, bool wide) {
    if (wide) {
        for (size_t i = 0; i + 2 <= size; i += 2) {
            if (!data[i] && !data[i + 1]) return i + 2;
        }
        return size;
    }
    for (size_t i = 0; i < size; i++) {
        if (!data[i]) return i + 1;
    }
    return size;
}

/* Offset of the picture bytes inside an APIC/PIC body, or 0 when the header
   before them does not parse (which includes the case of it running past the
   prefix we read). */
static size_t picture_prefix(const uint8_t *body, size_t size, bool v22, bool *front) {
    media_text_encoding_t encoding;
    if (size < 2 || !text_encoding(body[0], &encoding)) return 0;
    const bool wide = encoding == MEDIA_TEXT_UTF16 || encoding == MEDIA_TEXT_UTF16BE;

    size_t position = 1;
    if (v22) {
        position += 3;
    } else {
        position += skip_string(body + position, size - position, false);
    }
    if (position + 1 > size) return 0;

    const uint8_t picture_type = body[position++];
    position += skip_string(body + position, size - position, wide);
    if (position >= size) return 0;
    *front = picture_type == 3;
    return position;
}

static void parse_picture(media_tags_t *tags, const uint8_t *body, size_t size, bool v22) {
    bool front = false;
    const size_t prefix = picture_prefix(body, size, v22, &front);
    if (prefix) media_tags_set_cover(tags, body + prefix, size - prefix, front);
}

static void parse_frames(media_tags_t *tags, uint8_t *data, size_t size, uint8_t major) {
    const size_t header_bytes = major == 2 ? 6 : 10;
    const size_t id_bytes = major == 2 ? 3 : 4;

    for (size_t position = 0; position + header_bytes <= size;) {
        const char *id = (const char *)(data + position);
        if (!id[0]) return;

        size_t length;
        uint8_t flags = 0;
        if (major == 2) {
            length = (size_t)data[position + 3] << 16 | (size_t)data[position + 4] << 8 |
                     data[position + 5];
        } else {
            length = major == 4 ? synchsafe32(data + position + 4) : be32(data + position + 4);
            flags = data[position + 9];
        }
        position += header_bytes;
        if (length > size - position) return;

        uint8_t *body = data + position;
        size_t body_bytes = length;
        position += length;

        if (major == 3 && (flags & 0xC0)) continue;
        if (major == 3 && (flags & 0x20) && body_bytes) {
            body++;
            body_bytes--;
        }
        if (major == 4) {
            if (flags & 0x0C) continue;
            if ((flags & 0x40) && body_bytes) {
                body++;
                body_bytes--;
            }
            if ((flags & 0x01) && body_bytes >= 4) {
                body += 4;
                body_bytes -= 4;
            }
            if (flags & 0x02) body_bytes = unsynchronise(body, body_bytes);
        }
        if (!body_bytes) continue;

        if (memcmp(id, major == 2 ? "PIC" : "APIC", id_bytes) == 0) {
            parse_picture(tags, body, body_bytes, major == 2);
            continue;
        }

        const frame_map_t *frame =
            major == 2 ? lookup(id, kFrames2, sizeof(kFrames2) / sizeof(*kFrames2), id_bytes)
                       : lookup(id, kFrames3, sizeof(kFrames3) / sizeof(*kFrames3), id_bytes);
        media_text_encoding_t encoding;
        if (!frame || !text_encoding(body[0], &encoding)) continue;
        media_tags_set(tags, frame->field, body + 1, body_bytes - 1, encoding);
    }
}

static bool read_exact(media_buffer_t *reader, off_t at, void *out, size_t size) {
    mb_seek(reader, at);
    return mb_read(reader, out, size) == size;
}

/* Global unsynchronisation rewrites the byte stream, so frame lengths stop
   matching file offsets and the tag has to be taken in one piece. */
static void read_whole(media_buffer_t *reader, off_t body_at, size_t size, uint8_t flags,
                       uint8_t major, media_tags_t *tags) {
    void *owner = NULL;
    uint8_t *data = mb_read_alloc(reader, body_at, size, &owner);
    if (!data) return;

    size_t bytes = unsynchronise(data, size);
    size_t start = 0;
    if (flags & 0x40) {
        if (bytes < 4) {
            start = bytes;
        } else if (major == 4) {
            start = synchsafe32(data);
        } else {
            start = be32(data) + 4;
        }
    }
    if (start < bytes) parse_frames(tags, data + start, bytes - start, major);
    tags->cover_scanned = true;
    heap_caps_free(owner);
}

static void handle_picture(media_buffer_t *reader, media_tags_t *tags, off_t frame_at,
                           size_t length, bool v22, bool want_cover, uint8_t *scratch,
                           bool *have_front) {
    const size_t peek = length < ID3_SCRATCH_BYTES ? length : ID3_SCRATCH_BYTES;
    if (!read_exact(reader, frame_at, scratch, peek)) return;

    bool front = false;
    const size_t prefix = picture_prefix(scratch, peek, v22, &front);
    if (!prefix || prefix >= length) return;
    /* Recording where a frame that is not a picture sits would send a later
       pass off to read it for nothing. */
    if (prefix + 8 <= peek && media_cover_format_of(scratch + prefix, 8) == MEDIA_COVER_NONE) {
        return;
    }
    if (tags->cover_at.size && (!front || *have_front)) return;

    tags->cover_at.offset = frame_at + (off_t)prefix;
    tags->cover_at.size = (uint32_t)(length - prefix);
    *have_front = front;
    if (!want_cover) return;

    void *owner = NULL;
    uint8_t *data = mb_read_alloc(reader, tags->cover_at.offset, tags->cover_at.size, &owner);
    if (data) media_tags_adopt_cover(tags, owner, data, tags->cover_at.size, front);
}

/* Walks the frame list over the file instead of pulling the tag into memory:
   a picture body is the bulk of a tagged file and is either read straight into
   its final buffer or skipped outright. */
static void walk_frames(media_buffer_t *reader, off_t body_at, size_t size, uint8_t flags,
                        uint8_t major, media_tags_t *tags, bool want_cover) {
    const size_t header_bytes = major == 2 ? 6 : 10;
    const size_t id_bytes = major == 2 ? 3 : 4;

    size_t position = 0;
    if (flags & 0x40) {
        uint8_t extended[4];
        if (!read_exact(reader, body_at, extended, sizeof(extended))) return;
        position = major == 4 ? synchsafe32(extended) : be32(extended) + 4;
        if (position >= size) return;
    }

    uint8_t *scratch = heap_caps_malloc(ID3_SCRATCH_BYTES, MALLOC_CAP_SPIRAM);
    if (!scratch) scratch = heap_caps_malloc(ID3_SCRATCH_BYTES, MALLOC_CAP_DEFAULT);
    if (!scratch) return;

    bool have_front = false;
    while (position + header_bytes <= size) {
        uint8_t head[10];
        if (!read_exact(reader, body_at + (off_t)position, head, header_bytes)) break;
        if (!head[0]) break;

        size_t length;
        uint8_t frame_flags = 0;
        if (major == 2) {
            length = (size_t)head[3] << 16 | (size_t)head[4] << 8 | head[5];
        } else {
            length = major == 4 ? synchsafe32(head + 4) : be32(head + 4);
            frame_flags = head[9];
        }
        position += header_bytes;
        if (length > size - position) break;

        off_t frame_at = body_at + (off_t)position;
        position += length;
        if (!length) continue;

        if (major == 3 && (frame_flags & 0xC0)) continue;
        if (major == 3 && (frame_flags & 0x20)) {
            frame_at++;
            length--;
        }
        bool unsynchronised = false;
        if (major == 4) {
            if (frame_flags & 0x0C) continue;
            if (frame_flags & 0x40) {
                frame_at++;
                length--;
            }
            if ((frame_flags & 0x01) && length >= 4) {
                frame_at += 4;
                length -= 4;
            }
            unsynchronised = (frame_flags & 0x02) != 0;
        }
        if (!length) continue;

        const bool picture = memcmp(head, major == 2 ? "PIC" : "APIC", id_bytes) == 0;
        if (picture && !unsynchronised) {
            handle_picture(reader, tags, frame_at, length, major == 2, want_cover, scratch,
                           &have_front);
            continue;
        }

        const frame_map_t *frame =
            major == 2 ? lookup((const char *)head, kFrames2,
                                sizeof(kFrames2) / sizeof(*kFrames2), id_bytes)
                       : lookup((const char *)head, kFrames3,
                                sizeof(kFrames3) / sizeof(*kFrames3), id_bytes);
        if (!frame && !picture) continue;

        size_t body_bytes = length < ID3_SCRATCH_BYTES ? length : ID3_SCRATCH_BYTES;
        if (!read_exact(reader, frame_at, scratch, body_bytes)) break;
        if (unsynchronised) body_bytes = unsynchronise(scratch, body_bytes);
        if (!body_bytes) continue;

        if (picture) {
            parse_picture(tags, scratch, body_bytes, major == 2);
            continue;
        }
        media_text_encoding_t encoding;
        if (!text_encoding(scratch[0], &encoding)) continue;
        media_tags_set(tags, frame->field, scratch + 1, body_bytes - 1, encoding);
    }
    tags->cover_scanned = true;
    heap_caps_free(scratch);
}

bool media_tags_read_id3v2(media_buffer_t *reader, off_t offset, off_t *end, media_tags_t *tags,
                           bool want_cover) {
    uint8_t header[ID3_HEADER_BYTES];
    mb_seek(reader, offset);
    if (mb_read(reader, header, sizeof(header)) != sizeof(header)) return false;
    if (memcmp(header, "ID3", 3) != 0) return false;
    if ((header[6] | header[7] | header[8] | header[9]) & 0x80) return false;

    const uint8_t major = header[3];
    const size_t size = synchsafe32(header + 6);
    if (end) *end = offset + (off_t)ID3_HEADER_BYTES + (off_t)size + ((header[5] & 0x10) ? 10 : 0);
    if (!tags || !size || size > ID3_MAX_TAG_BYTES || major < 2 || major > 4) return true;

    const off_t body_at = offset + (off_t)ID3_HEADER_BYTES;
    if (header[5] & 0x80) {
        read_whole(reader, body_at, size, header[5], major, tags);
    } else {
        walk_frames(reader, body_at, size, header[5], major, tags, want_cover);
    }
    return true;
}

bool media_tags_read_id3v1(media_buffer_t *reader, off_t end, media_tags_t *tags) {
    uint8_t block[ID3V1_BYTES];
    if (end < ID3V1_BYTES) return false;
    mb_seek(reader, end - ID3V1_BYTES);
    if (mb_read(reader, block, sizeof(block)) != sizeof(block)) return false;
    if (memcmp(block, "TAG", 3) != 0) return false;

    media_tags_set(tags, MEDIA_TAG_TITLE, block + 3, 30, MEDIA_TEXT_LATIN1);
    media_tags_set(tags, MEDIA_TAG_ARTIST, block + 33, 30, MEDIA_TEXT_LATIN1);
    media_tags_set(tags, MEDIA_TAG_ALBUM, block + 63, 30, MEDIA_TEXT_LATIN1);
    media_tags_set(tags, MEDIA_TAG_DATE, block + 93, 4, MEDIA_TEXT_LATIN1);
    if (!block[125] && block[126]) media_tags_set_number(tags, MEDIA_TAG_TRACK, block[126], 0);
    return true;
}
