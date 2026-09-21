/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_ARENA_ALIGNMENT 64
#define MB_READ_ALIGNMENT 64
#define MB_NO_REF 0

typedef struct {
    uint8_t *data;
    size_t size;
    /* Header and tag reads only: no read-ahead task, mb_read() is served from a
       window instead, and mb_view() always fails. */
    bool direct;
} media_arena_t;

typedef struct media_buffer media_buffer_t;

size_t mb_arena_max_view(const media_arena_t *arena);

media_buffer_t *mb_open(const char *path, const media_arena_t *arena);
void mb_close(media_buffer_t *buffer);

off_t mb_size(const media_buffer_t *buffer);
off_t mb_tell(const media_buffer_t *buffer);
size_t mb_read(media_buffer_t *buffer, void *out, size_t size);

/* Reads `size` bytes at `offset` into a fresh PSRAM allocation. FATFS copies
   the leading partial sector through its own window and then DMAs the rest
   straight to the caller, so the transfer only runs at full speed when the
   destination carries the same MB_READ_ALIGNMENT phase as the file offset
   (6.4 vs 11.3 MB/s measured on a Tab5 SD card). The buffer is over-allocated
   to place the data at that phase: the return value is the data, *out_owner
   the pointer to heap_caps_free(). */
uint8_t *mb_read_alloc(media_buffer_t *buffer, off_t offset, size_t size, void **out_owner);
off_t mb_seek(media_buffer_t *buffer, off_t offset);
void mb_skip(media_buffer_t *buffer, off_t delta);
void mb_set_readahead(media_buffer_t *buffer, bool enabled);

const uint8_t *mb_view(media_buffer_t *buffer, size_t size, uint32_t *ref);
const uint8_t *mb_view_at(media_buffer_t *buffer, off_t offset, size_t size, uint32_t *ref);
void mb_release(media_buffer_t *buffer, uint32_t ref);
void mb_release_all(media_buffer_t *buffer);
void mb_interrupt(media_buffer_t *buffer, bool interrupted);

#ifdef __cplusplus
}
#endif
