/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mpeg2_dec.h"


#define WORK_BYTES 245760

static void *alloc_cb(void *ctx, size_t bytes) {
    (void)ctx;
    return aligned_alloc(64, (bytes + 63) & ~(size_t)63);
}

static void free_cb(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint32_t count;
} sem_t_;

static void *sem_create_cb(void *ctx, uint32_t max, uint32_t initial) {
    (void)ctx;
    (void)max;
    sem_t_ *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->count = initial;
    return s;
}

static void sem_delete_cb(void *ctx, void *sem) {
    (void)ctx;
    sem_t_ *s = sem;
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}

static void sem_take_cb(void *ctx, void *sem) {
    (void)ctx;
    sem_t_ *s = sem;
    pthread_mutex_lock(&s->lock);
    while (!s->count) pthread_cond_wait(&s->cond, &s->lock);
    s->count--;
    pthread_mutex_unlock(&s->lock);
}

static void sem_give_cb(void *ctx, void *sem) {
    (void)ctx;
    sem_t_ *s = sem;
    pthread_mutex_lock(&s->lock);
    s->count++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

typedef struct {
    void (*entry)(void *);
    void *arg;
} spawn_t;

static void *thread_main(void *p) {
    spawn_t *s = p;
    s->entry(s->arg);
    free(s);
    return NULL;
}

static bool spawn_cb(void *ctx, void (*entry)(void *), void *arg) {
    (void)ctx;
    spawn_t *s = malloc(sizeof(*s));
    s->entry = entry;
    s->arg = arg;
    pthread_t thread;
    if (pthread_create(&thread, NULL, thread_main, s) != 0) return false;
    pthread_detach(thread);
    return true;
}

static const vdec_threads_t kThreads = {
    .sem_create = sem_create_cb,
    .sem_delete = sem_delete_cb,
    .sem_take = sem_take_cb,
    .sem_give = sem_give_cb,
    .spawn = spawn_cb,
};

static const uint8_t *next_start(const uint8_t *p, const uint8_t *end) {
    for (; p + 4 <= end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) return p;
    }
    return end;
}

static void write_picture(FILE *out, const mpeg2_dec_picture_t *pic, uint8_t *plane) {
    const mpeg2_dec_stream_info_t *in = &pic->info;
    const size_t stride = (size_t)in->coded_width * 3 / 2;
    for (int y = 0; y < in->height; y++) {
        const uint8_t *row = pic->packed + (size_t)y * stride;
        for (int x = 0; x < in->width; x++) plane[x] = row[3 * (x >> 1) + 1 + (x & 1)];
        fwrite(plane, 1, in->width, out);
    }
    const int cw = (in->width + 1) / 2, ch = (in->height + 1) / 2;
    for (int comp = 0; comp < 2; comp++) {
        for (int y = 0; y < ch; y++) {
            const uint8_t *row = pic->packed + ((size_t)y * 2 + comp) * stride;
            for (int x = 0; x < cw; x++) plane[x] = row[3 * x];
            fwrite(plane, 1, cw, out);
        }
    }
}

static void emit(mpeg2_dec_t *dec, FILE *out, uint8_t *plane, bool hash, bool verbose, int *frames,
                 int *errors) {
    mpeg2_dec_picture_t pic;
    while (mpeg2_dec_output(dec, &pic)) {
        if (verbose) {
            fprintf(stderr, "out frame %d tag %lld type %d%s\n", *frames, (long long)pic.tag,
                    pic.type, pic.concealed ? " concealed" : "");
        }
        if (out) write_picture(out, &pic, plane);
        if (hash) {
            uint32_t h = 2166136261u;
            for (size_t i = 0; i < pic.packed_bytes; i++) h = (h ^ pic.packed[i]) * 16777619u;
            printf("[MPEG2V] f=%d h=%08x\n", *frames, h);
        }
        if (pic.concealed) (*errors)++;
        mpeg2_dec_release(dec, pic.id);
        (*frames)++;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s input.m2v [output.yuv]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    const long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, in) != (size_t)size) return 1;
    fclose(in);
    FILE *out = argc > 2 ? fopen(argv[2], "wb") : NULL;

    uint8_t *work = aligned_alloc(64, WORK_BYTES);
    mpeg2_dec_config_t config = {
        .alloc = alloc_cb,
        .free = free_cb,
        .work = work,
        .work_bytes = WORK_BYTES,
        .max_mbs = 3600,
        .max_side = 1280,
        .held_pictures = 1,
        .threads = getenv("MPEG2_THREADS") ? &kThreads : NULL,
    };
    mpeg2_dec_t *dec = mpeg2_dec_create(&config);
    if (!dec) {
        fprintf(stderr, "create failed\n");
        return 1;
    }
    uint8_t *plane = malloc(4096);

    const char *loops_env = getenv("MPEG2_LOOPS");
    int loops = loops_env ? atoi(loops_env) : 1;
    const bool verbose = getenv("MPEG2_VERBOSE") != NULL;
    const bool hash = getenv("MPEG2_HASH") != NULL;
    int frames = 0, errors = 0, aus = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
restart:;
    const uint8_t *end = data + size;
    const uint8_t *p = next_start(data, end);
    const uint8_t *au = p;
    bool au_has_slice = false;
    for (;;) {
        bool boundary = p >= end;
        if (!boundary && au_has_slice) {
            const uint8_t code = p[3];
            boundary = code == 0x00 || code == 0xB3 || code == 0xB8;
        }
        if (boundary && au < p) {
            const mpeg2_dec_result_t r = mpeg2_dec_decode(dec, au, (size_t)(p - au), aus);
            if (verbose) {
                const char *e = mpeg2_dec_error(dec);
                fprintf(stderr, "au %d bytes %d result %d%s%s\n", aus, (int)(p - au), (int)r,
                        e ? " " : "", e ? e : "");
            }
            aus++;
            if (r != MPEG2_DEC_OK && r != MPEG2_DEC_NO_PICTURE) {
                const char *e = mpeg2_dec_error(dec);
                fprintf(stderr, "au %d: result %d %s\n", aus - 1, (int)r, e ? e : "");
                errors++;
                if (r == MPEG2_DEC_UNSUPPORTED) break;
            }
            emit(dec, out, plane, hash, verbose, &frames, &errors);
            au = p;
            au_has_slice = false;
        }
        if (p >= end) break;
        if (p[3] >= 0x01 && p[3] <= 0xAF) au_has_slice = true;
        p = next_start(p + 3, end);
    }
    mpeg2_dec_drain(dec);
    emit(dec, out, plane, hash, verbose, &frames, &errors);
    if (--loops > 0) {
        mpeg2_dec_flush(dec);
        goto restart;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "%d frames, %d with errors, %.1f fps\n", frames, errors, frames / secs);
    if (out) fclose(out);
    mpeg2_dec_destroy(dec);
    free(work);
    free(plane);
    free(data);
    return errors ? 1 : 0;
}
