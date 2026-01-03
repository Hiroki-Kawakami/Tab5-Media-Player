#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#define BR_CHUNK_SIZE  (128 * 1024)
#define BR_CHUNK_NUM   (32)

typedef struct buffered_reader buffered_reader_t;
buffered_reader_t *br_open(const char *path);
void br_close(buffered_reader_t *reader);
size_t br_read(buffered_reader_t *reader, void *buffer, size_t size);
off_t br_lseek(buffered_reader_t *reader, off_t offset, int whence);
void br_set_preload_enable(buffered_reader_t *reader, bool enable);
