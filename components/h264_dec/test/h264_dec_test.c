/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "h264_dec.h"

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

static const h264_dec_threads_t kThreads = {
    .sem_create = sem_create_cb,
    .sem_delete = sem_delete_cb,
    .sem_take = sem_take_cb,
    .sem_give = sem_give_cb,
    .spawn = spawn_cb,
};

static const uint8_t *next_start(const uint8_t *p, const uint8_t *end) {
    for (; p + 3 <= end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) return p;
    }
    return end;
}

static bool first_slice_of_picture(const uint8_t *nal, const uint8_t *end) {
    const uint8_t type = nal[0] & 0x1F;
    if (type != 1 && type != 5) return false;
    return nal + 1 < end && (nal[1] & 0x80);
}

static void write_picture(FILE *out, const h264_dec_picture_t *pic, uint8_t *plane) {
    const h264_dec_stream_info_t *in = &pic->info;
    const size_t stride = (size_t)in->coded_width * 3 / 2;
    for (int y = 0; y < in->height; y++) {
        const uint8_t *row = pic->packed + (size_t)(y + in->crop_top) * stride;
        for (int x = 0; x < in->width; x++) {
            const int xx = x + in->crop_left;
            plane[x] = row[3 * (xx >> 1) + 1 + (xx & 1)];
        }
        fwrite(plane, 1, in->width, out);
    }
    for (int comp = 0; comp < 2; comp++) {
        for (int y = 0; y < in->height / 2; y++) {
            const int yy = y + in->crop_top / 2;
            const uint8_t *row = pic->packed + ((size_t)yy * 2 + comp) * stride;
            for (int x = 0; x < in->width / 2; x++) plane[x] = row[3 * (x + in->crop_left / 2)];
            fwrite(plane, 1, in->width / 2, out);
        }
    }
}

static void emit(h264_dec_t *dec, FILE *out, uint8_t *plane, bool hash, bool verbose, int *frames,
                 int *errors) {
    h264_dec_picture_t pic;
    while (h264_dec_output(dec, &pic)) {
        if (verbose) fprintf(stderr, "out frame %d tag %lld\n", *frames, (long long)pic.tag);
        if (out) write_picture(out, &pic, plane);
        if (hash) {
            uint32_t h = 2166136261u;
            for (size_t i = 0; i < pic.packed_bytes; i++) h = (h ^ pic.packed[i]) * 16777619u;
            printf("[H264V] f=%d h=%08x\n", *frames, h);
        }
        if (pic.concealed) (*errors)++;
        h264_dec_release(dec, pic.id);
        (*frames)++;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s input.h264 [output.yuv]\n", argv[0]);
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
    h264_dec_config_t config = {
        .alloc = alloc_cb,
        .free = free_cb,
        .work = work,
        .work_bytes = WORK_BYTES,
        .max_mbs = 3600,
        .max_side = 1280,
        .held_pictures = 1,
        .threads = getenv("H264_THREADS") ? &kThreads : NULL,
    };
    h264_dec_t *dec = h264_dec_create(&config);
    if (!dec) {
        fprintf(stderr, "create failed\n");
        return 1;
    }
    uint8_t *plane = malloc(4096);

    const char *loops_env = getenv("H264_LOOPS");
    int loops = loops_env ? atoi(loops_env) : 1;
restart:;
    const uint8_t *end = data + size;
    const uint8_t *p = next_start(data, end);
    const uint8_t *au = p;
    bool au_has_vcl = false;
    int frames = 0, errors = 0, aus = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const bool verbose = getenv("H264_VERBOSE") != NULL;
    const bool hash = getenv("H264_HASH") != NULL;
    for (;;) {
        const uint8_t *nal = p < end ? p + 3 : end;
        bool boundary = p >= end;
        if (!boundary && au_has_vcl) {
            const uint8_t type = nal[0] & 0x1F;
            boundary = first_slice_of_picture(nal, end) || (type >= 6 && type <= 9);
        }
        if (boundary && au < p) {
            const h264_dec_result_t r = h264_dec_decode(dec, au, (size_t)(p - au), 0, aus);
            if (verbose) {
                const char *e = h264_dec_error(dec);
                fprintf(stderr, "au %d bytes %d result %d%s%s\n", aus, (int)(p - au), (int)r,
                        e ? " " : "", e ? e : "");
            }
            aus++;
            if (r != H264_DEC_OK && r != H264_DEC_NO_PICTURE) {
                const char *e = h264_dec_error(dec);
                fprintf(stderr, "frame %d: result %d %s\n", frames, (int)r, e ? e : "");
                errors++;
                if (r == H264_DEC_UNSUPPORTED) break;
            }
            emit(dec, out, plane, hash, verbose, &frames, &errors);
            au = p;
            au_has_vcl = false;
        }
        if (p >= end) break;
        const uint8_t type = nal[0] & 0x1F;
        if (type == 1 || type == 5) au_has_vcl = true;
        p = next_start(nal, end);
    }
    h264_dec_drain(dec);
    emit(dec, out, plane, hash, verbose, &frames, &errors);
    if (--loops > 0) {
        h264_dec_flush(dec);
        goto restart;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "%d frames, %d with errors, %.1f fps\n", frames, errors, frames / secs);
    if (out) fclose(out);
    h264_dec_destroy(dec);
    free(work);
    free(plane);
    free(data);
    return errors ? 1 : 0;
}
