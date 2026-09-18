/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"
#include "kernels.h"

void h264_pack_rows(uint8_t *dst, uint32_t dst_stride, const uint8_t *y, uint32_t y_stride,
                    const uint8_t *u, const uint8_t *v, uint32_t c_stride, uint32_t width,
                    uint32_t rows) {
    const int blocks = (int)((width / 2 + 15) / 16);
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *cs = (r & 1 ? v : u) + (r >> 1) * c_stride;
        h264_k_pack(cs, y + r * y_stride, dst + r * dst_stride, blocks);
    }
}

static void jobs_push(job_queue_t *q, const row_job_t *job) {
    const unsigned head = atomic_load(&q->head);
    q->jobs[head & (QUEUE_SIZE - 1)] = *job;
    atomic_store(&q->head, head + 1);
}

static bool jobs_pop(job_queue_t *q, row_job_t *job) {
    const unsigned tail = atomic_load(&q->tail);
    if (tail == atomic_load(&q->head)) return false;
    *job = q->jobs[tail & (QUEUE_SIZE - 1)];
    atomic_store(&q->tail, tail + 1);
    return true;
}

static void slots_push(slot_queue_t *q, int8_t slot) {
    const unsigned head = atomic_load(&q->head);
    q->slots[head & (QUEUE_SIZE - 1)] = slot;
    atomic_store(&q->head, head + 1);
}

static int8_t slots_pop(slot_queue_t *q) {
    const unsigned tail = atomic_load(&q->tail);
    if (tail == atomic_load(&q->head)) return -1;
    const int8_t slot = q->slots[tail & (QUEUE_SIZE - 1)];
    atomic_store(&q->tail, tail + 1);
    return slot;
}

void h264_store_col_row(struct h264_dec *dec, const rowbuf_t *row, uint8_t *col, uint32_t mb_y) {
    const int blocks = dec->col_blocks;
    colblk_t *out = (colblk_t *)col + (size_t)mb_y * dec->mb_w * blocks;
    for (uint32_t x = 0; x < dec->mb_w; x++, out += blocks) {
        const mbinfo_t *m = mb_at(row, x, dec->mb_stride);
        const bool inter = m->kind == MB_INTER || m->kind == MB_SKIP;
        for (int b = 0; b < blocks; b++) {
            const int r = blocks == 4 ? h264_col_corner[b] : b;
            const int blk8 = ((r >> 3) << 1) | ((r & 3) >> 1);
            if (!inter) {
                out[b].mv[0] = out[b].mv[1] = 0;
                out[b].ref = -1;
                out[b].pic = NO_PIC;
                continue;
            }
            const int list = m->m[0].ref[blk8] >= 0 ? 0 : 1;
            const mbmotion_t *mm = &m->m[list];
            out[b].mv[0] = mm->mv[r][0];
            out[b].mv[1] = mm->mv[r][1];
            out[b].ref = mm->ref[blk8];
            out[b].pic = mm->refpic[blk8];
        }
    }
}

static void flush_row(struct h264_dec *dec, const rowbuf_t *row, uint8_t *frame, uint32_t mb_y) {
    {
        PROF_START(dec);
        h264_pack_rows(dec->packed_row, dec->packed_stride, row->y, dec->luma_stride, row->u,
                       row->v, dec->chroma_stride, dec->width, 16);
        PROF_STOP(dec, H264_PROF_PACK);
    }
    PROF_START(dec);
    memcpy(frame + (size_t)mb_y * 16 * dec->packed_stride, dec->packed_row,
           (size_t)dec->packed_stride * 16);
    PROF_STOP(dec, H264_PROF_FLUSH);
}

static void release_slot(struct h264_dec *dec, int8_t slot) {
    slots_push(&dec->free_slots, slot);
    if (dec->threaded) dec->config.threads->sem_give(dec->config.threads->ctx, dec->sem_free);
}

static void process_job(struct h264_dec *dec, const row_job_t *job) {
    rowbuf_t *cur = &dec->rows[job->slot];
    rowbuf_t *prev = job->prev >= 0 ? &dec->rows[job->prev] : NULL;
    const size_t w = dec->width;
    const size_t cw = w / 2;
    const size_t ls = dec->luma_stride;
    const size_t cs = dec->chroma_stride;

    if (prev) {
        for (int k = 0; k < LUMA_ABOVE; k++) {
            memcpy(cur->y - (LUMA_ABOVE - k) * ls, prev->y + (16 - LUMA_ABOVE + k) * ls, w);
        }
        for (int k = 0; k < CHROMA_ABOVE; k++) {
            memcpy(cur->u - (CHROMA_ABOVE - k) * cs, prev->u + (8 - CHROMA_ABOVE + k) * cs, cw);
            memcpy(cur->v - (CHROMA_ABOVE - k) * cs, prev->v + (8 - CHROMA_ABOVE + k) * cs, cw);
        }
    }
    {
        PROF_START(dec);
        h264_deblock_row(dec, cur, prev, job->mb_y);
        PROF_STOP(dec, H264_PROF_DEBLOCK);
    }
    if (prev) {
        for (int k = 1; k < LUMA_ABOVE; k++) {
            memcpy(prev->y + (16 - LUMA_ABOVE + k) * ls, cur->y - (LUMA_ABOVE - k) * ls, w);
        }
        memcpy(prev->u + 7 * cs, cur->u - cs, cw);
        memcpy(prev->v + 7 * cs, cur->v - cs, cw);
        flush_row(dec, prev, job->frame, job->mb_y - 1u);
        if (job->col) h264_store_col_row(dec, prev, job->col, job->mb_y - 1u);
        release_slot(dec, job->prev);
    }
    if (job->last) {
        flush_row(dec, cur, job->frame, job->mb_y);
        if (job->col) h264_store_col_row(dec, cur, job->col, job->mb_y);
        release_slot(dec, job->slot);
        if (dec->threaded) dec->config.threads->sem_give(dec->config.threads->ctx, dec->sem_done);
    }
}

static void worker(void *arg) {
    struct h264_dec *dec = arg;
    const h264_dec_threads_t *t = dec->config.threads;
    h264_k_prepare();
    for (;;) {
        t->sem_take(t->ctx, dec->sem_work);
        if (atomic_load(&dec->win_claimed) < atomic_load(&dec->win_goal)) h264_window_fill(dec);
        row_job_t job;
        if (!jobs_pop(&dec->work, &job)) continue;
        if (job.kind == JOB_STOP) break;
        process_job(dec, &job);
    }
    t->sem_give(t->ctx, dec->sem_done);
}

void h264_window_request(struct h264_dec *dec, int target) {
    atomic_store(&dec->win_goal, target);
    if (!dec->threaded) {
        h264_window_fill(dec);
        return;
    }
    dec->config.threads->sem_give(dec->config.threads->ctx, dec->sem_work);
}

void h264_window_wait(struct h264_dec *dec, int rows) {
    if (!dec->threaded) return;
    {
        PROF_START(dec);
        while (h264_window_fill_one(dec, rows, NULL)) {}
        PROF_STOP(dec, H264_PROF_WINDOW);
    }
    for (;;) {
        const int busy = atomic_load(&dec->win_busy);
        if (busy < 0 || busy >= rows) break;
        PROF_START(dec);
        dec->config.threads->sem_take(dec->config.threads->ctx, dec->sem_window);
        PROF_STOP(dec, H264_PROF_WAIT);
    }
}

static int8_t take_slot(struct h264_dec *dec) {
    if (dec->threaded) {
        if (dec->win_frame && dec->work.head != dec->work.tail) {
            PROF_START(dec);
            while (h264_window_fill_one(dec, atomic_load(&dec->win_goal), NULL)) {
                if (atomic_load(&dec->work.tail) == atomic_load(&dec->work.head)) break;
            }
            PROF_STOP(dec, H264_PROF_WINDOW);
        }
        PROF_START(dec);
        dec->config.threads->sem_take(dec->config.threads->ctx, dec->sem_free);
        PROF_STOP(dec, H264_PROF_WAIT);
    }
    return slots_pop(&dec->free_slots);
}

static void submit(struct h264_dec *dec, const row_job_t *job) {
    if (!dec->threaded) {
        process_job(dec, job);
        return;
    }
    jobs_push(&dec->work, job);
    dec->config.threads->sem_give(dec->config.threads->ctx, dec->sem_work);
}

void h264_rows_reset(struct h264_dec *dec) {
    const h264_dec_threads_t *t = dec->config.threads;
    atomic_store(&dec->free_slots.head, 0);
    atomic_store(&dec->free_slots.tail, 0);
    if (dec->threaded) {
        t->sem_delete(t->ctx, dec->sem_free);
        dec->sem_free = t->sem_create(t->ctx, QUEUE_SIZE, 0);
    }
    for (uint8_t i = 0; i < dec->slot_count; i++) release_slot(dec, (int8_t)i);
    dec->cur_slot = -1;
    dec->above_slot = -1;
}

bool h264_rows_start(struct h264_dec *dec) {
    const h264_dec_threads_t *t = dec->config.threads;
    if (!t) return true;
    dec->sem_work = t->sem_create(t->ctx, QUEUE_SIZE, 0);
    dec->sem_free = t->sem_create(t->ctx, QUEUE_SIZE, 0);
    dec->sem_done = t->sem_create(t->ctx, 1, 0);
    dec->sem_window = t->sem_create(t->ctx, 1024, 0);
    if (!dec->sem_work || !dec->sem_free || !dec->sem_done || !dec->sem_window) return false;
    dec->threaded = true;
    if (!t->spawn(t->ctx, worker, dec)) {
        dec->threaded = false;
        return false;
    }
    return true;
}

void h264_rows_stop(struct h264_dec *dec) {
    const h264_dec_threads_t *t = dec->config.threads;
    if (dec->threaded) {
        const row_job_t stop = { .kind = JOB_STOP };
        jobs_push(&dec->work, &stop);
        t->sem_give(t->ctx, dec->sem_work);
        t->sem_take(t->ctx, dec->sem_done);
        dec->threaded = false;
    }
    if (!t) return;
    if (dec->sem_work) t->sem_delete(t->ctx, dec->sem_work);
    if (dec->sem_free) t->sem_delete(t->ctx, dec->sem_free);
    if (dec->sem_done) t->sem_delete(t->ctx, dec->sem_done);
    if (dec->sem_window) t->sem_delete(t->ctx, dec->sem_window);
    dec->sem_work = dec->sem_free = dec->sem_done = dec->sem_window = NULL;
}

void h264_rows_begin_picture(struct h264_dec *dec) {
    dec->cur_slot = take_slot(dec);
    dec->above_slot = -1;
}

void h264_row_done(struct h264_dec *dec, uint32_t mb_y) {
    const rowbuf_t *cur = &dec->rows[dec->cur_slot];
    const bool last = mb_y == (uint32_t)dec->mb_h - 1;
    const size_t w = dec->width;
    const size_t cw = w / 2;
    if (!last) {
        memcpy(dec->top_y, cur->y + 15 * dec->luma_stride, w);
        memcpy(dec->top_u, cur->u + 7 * dec->chroma_stride, cw);
        memcpy(dec->top_v, cur->v + 7 * dec->chroma_stride, cw);
    }
    const row_job_t job = {
        .frame = dec->cur->data,
        .col = dec->col_blocks && dec->first_slice.nal_ref_idc ? dec->cur->col : NULL,
        .mb_y = (uint16_t)mb_y,
        .slot = dec->cur_slot,
        .prev = dec->above_slot,
        .kind = JOB_ROW,
        .last = last,
    };
    submit(dec, &job);
    if (last) {
        if (dec->threaded) {
            PROF_START(dec);
            dec->config.threads->sem_take(dec->config.threads->ctx, dec->sem_done);
            PROF_STOP(dec, H264_PROF_WAIT);
        }
        dec->cur_slot = -1;
        dec->above_slot = -1;
        return;
    }
    dec->above_slot = dec->cur_slot;
    dec->cur_slot = take_slot(dec);
    rowbuf_t *next = &dec->rows[dec->cur_slot];
    memcpy(next->y - dec->luma_stride, dec->top_y, w);
    memcpy(next->u - dec->chroma_stride, dec->top_u, cw);
    memcpy(next->v - dec->chroma_stride, dec->top_v, cw);
}
