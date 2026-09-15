/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct buffered_reader buffered_reader_t;

buffered_reader_t *br_open(const char *path);
void br_close(buffered_reader_t *reader);

off_t br_size(const buffered_reader_t *reader);
off_t br_tell(const buffered_reader_t *reader);
size_t br_read(buffered_reader_t *reader, void *buffer, size_t size);
off_t br_seek(buffered_reader_t *reader, off_t offset);
void br_skip(buffered_reader_t *reader, off_t delta);
void br_set_readahead(buffered_reader_t *reader, bool enabled);
