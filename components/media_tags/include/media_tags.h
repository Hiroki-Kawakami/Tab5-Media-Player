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
    uint8_t *data;
    uint32_t size;
    media_cover_format_t format;
    bool front;
} media_cover_t;

typedef struct {
    char title[MEDIA_TAG_TEXT_BYTES];
    char artist[MEDIA_TAG_TEXT_BYTES];
    char album[MEDIA_TAG_TEXT_BYTES];
    char album_artist[MEDIA_TAG_TEXT_BYTES];
    char track[MEDIA_TAG_SHORT_BYTES];
    char date[MEDIA_TAG_SHORT_BYTES];
    media_cover_t cover;
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

void media_tags_free(media_tags_t *tags);

/* Reads the ID3v2 tag at `offset`, if any. `*end` is left alone when there is
 * no readable tag, so a caller can use it as the start of the audio data. */
bool media_tags_read_id3v2(media_buffer_t *reader, off_t offset, off_t *end, media_tags_t *tags);

/* `end` is the end of the 128 byte ID3v1 block, i.e. the end of the file. */
bool media_tags_read_id3v1(media_buffer_t *reader, off_t end, media_tags_t *tags);

#ifdef __cplusplus
}
#endif
