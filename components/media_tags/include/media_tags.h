/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "media_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_TAG_TEXT_BYTES 128
#define MEDIA_TAG_SHORT_BYTES 16
#define MEDIA_COVER_MAX_BYTES (2 * 1024 * 1024)

typedef enum {
    MEDIA_TAG_TITLE,
    MEDIA_TAG_ARTIST,
    MEDIA_TAG_ALBUM,
    MEDIA_TAG_ALBUM_ARTIST,
    MEDIA_TAG_TRACK,
    MEDIA_TAG_DATE,
    MEDIA_TAG_COUNT,
} media_tag_field_t;

typedef enum {
    MEDIA_TEXT_LATIN1,
    MEDIA_TEXT_UTF16,
    MEDIA_TEXT_UTF16BE,
    MEDIA_TEXT_UTF8,
} media_text_encoding_t;

typedef enum {
    MEDIA_COVER_NONE,
    MEDIA_COVER_JPEG,
    MEDIA_COVER_PNG,
} media_cover_format_t;

typedef struct {
    uint8_t *owner;   /* allocation to free; `data` points into it */
    uint8_t *data;
    uint32_t size;
    media_cover_format_t format;
    bool front;
} media_cover_t;

/* Where the picture sits in the file. Filled even when the body was skipped,
   so a later pass can read just those bytes instead of the whole tag. */
typedef struct {
    off_t offset;
    uint32_t size;
} media_cover_at_t;

typedef struct {
    char title[MEDIA_TAG_TEXT_BYTES];
    char artist[MEDIA_TAG_TEXT_BYTES];
    char album[MEDIA_TAG_TEXT_BYTES];
    char album_artist[MEDIA_TAG_TEXT_BYTES];
    char track[MEDIA_TAG_SHORT_BYTES];
    char date[MEDIA_TAG_SHORT_BYTES];
    media_cover_t cover;
    media_cover_at_t cover_at;
    /* The pictures were enumerated, so an empty `cover`/`cover_at` means the
       file has none rather than that nobody looked. */
    bool cover_scanned;
} media_tags_t;

const char *media_tags_get(const media_tags_t *tags, media_tag_field_t field);
bool media_tags_empty(const media_tags_t *tags);

/* Converts to UTF-8 and stores it; the first non-empty value of a field wins,
 * so parsers may walk from the most to the least preferred source. */
void media_tags_set(media_tags_t *tags, media_tag_field_t field, const void *bytes, size_t size,
                    media_text_encoding_t encoding);
void media_tags_set_number(media_tags_t *tags, media_tag_field_t field, uint32_t value,
                           uint32_t total);

/* Keeps a copy of the picture when it is a JPEG or a PNG within the size
 * limit. A front cover replaces a picture stored without that hint. */
void media_tags_set_cover(media_tags_t *tags, const void *data, size_t size, bool front);

/* JPEG/PNG magic, so a caller can tell a picture from any other payload. */
media_cover_format_t media_cover_format_of(const void *data, size_t size);

/* Same rules, but takes ownership of `owner` instead of copying: `data`/`size`
   describe the picture inside it. `owner` is freed when the picture is
   rejected, so the caller hands it over either way. */
void media_tags_adopt_cover(media_tags_t *tags, uint8_t *owner, uint8_t *data, size_t size,
                            bool front);

/* Hands the picture buffer to the caller; the tag stops owning it. */
media_cover_t media_tags_take_cover(media_tags_t *tags);

void media_tags_free(media_tags_t *tags);

/* Reads the ID3v2 tag at `offset`, if any. `*end` is left alone when there is
 * no readable tag, so a caller can use it as the start of the audio data.
 * With `want_cover` false the picture body is located but never read, which is
 * nearly the whole cost of a tag on a file with cover art; `tags->cover_at`
 * then says where to find it later. */
bool media_tags_read_id3v2(media_buffer_t *reader, off_t offset, off_t *end, media_tags_t *tags,
                           bool want_cover);

/* `end` is the end of the 128 byte ID3v1 block, i.e. the end of the file. */
bool media_tags_read_id3v1(media_buffer_t *reader, off_t end, media_tags_t *tags);

#ifdef __cplusplus
}
#endif
