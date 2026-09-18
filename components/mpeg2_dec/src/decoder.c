/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mpeg2_internal.h"
#include "vdec_kernels.h"

static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end) {
    while (p + 3 <= end) {
        const uint8_t *z = memchr(p, 0, (size_t)(end - p - 2));
        if (!z) return end;
        if (z[1] == 0 && z[2] == 1) return z;
        p = z + 1;
    }
    return end;
}

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} unit_iter_t;

static bool next_unit(unit_iter_t *it, uint8_t *code, const uint8_t **body, size_t *len) {
    const uint8_t *sc = find_start_code(it->p, it->end);
    if (sc + 4 > it->end) {
        it->p = it->end;
        return false;
    }
    *code = sc[3];
    *body = sc + 4;
    const uint8_t *next = find_start_code(sc + 4, it->end);
    *len = (size_t)(next - *body);
    it->p = next;
    return true;
}

void *mpeg2_work_alloc(struct mpeg2_dec *dec, size_t bytes) {
    const size_t align = MPEG2_DEC_WORK_ALIGNMENT;
    const size_t start = (dec->work_used + align - 1) & ~(align - 1);
    if (start + bytes > dec->config.work_bytes) return NULL;
    dec->work_used = start + bytes;
    return dec->work_base + start;
}

static bool frame_free(const mpeg2_dec_t *dec, const frame_t *f) {
    return !f->waiting && !f->queued && atomic_load(&f->holds) == 0 && f != dec->old_ref &&
           f != dec->new_ref && f != dec->cur;
}

static bool frames_held(const mpeg2_dec_t *dec) {
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        if (atomic_load(&dec->frames[i].holds)) return true;
    }
    return false;
}

static void release_frames(mpeg2_dec_t *dec) {
    if (dec->frame_block) dec->config.free(dec->config.ctx, dec->frame_block);
    dec->frame_block = NULL;
    memset(dec->frames, 0, sizeof(dec->frames));
    dec->pool_size = 0;
    dec->old_ref = dec->new_ref = dec->cur = NULL;
    dec->outq_head = dec->outq_count = 0;
    dec->in_picture = false;
}

static worker_t *setup_worker(mpeg2_dec_t *dec) {
    worker_t *w = mpeg2_work_alloc(dec, sizeof(worker_t));
    if (!w) return NULL;
    memset(w, 0, sizeof(*w));
    w->dec = dec;
    w->bits_buf = mpeg2_work_alloc(dec, BITS_CHUNK + BITS_PADDING);
    w->y = mpeg2_work_alloc(dec, (size_t)dec->ystride * 16);
    w->u = mpeg2_work_alloc(dec, (size_t)dec->cstride * 8);
    w->v = mpeg2_work_alloc(dec, (size_t)dec->cstride * 8);
    uint8_t *ref = mpeg2_work_alloc(dec, 16 + 17 * MC_STRIDE + 32);
    w->ref_y = ref ? ref + 16 : NULL;
    w->ref_u = mpeg2_work_alloc(dec, 10 * 16 + 16);
    w->ref_v = mpeg2_work_alloc(dec, 10 * 16 + 16);
    for (int i = 0; i < 2; i++) {
        w->pred_y[i] = mpeg2_work_alloc(dec, 16 * MC_STRIDE + 16);
        w->pred_u[i] = mpeg2_work_alloc(dec, 8 * 16 + 16);
        w->pred_v[i] = mpeg2_work_alloc(dec, 8 * 16 + 16);
        if (!w->pred_y[i] || !w->pred_u[i] || !w->pred_v[i]) return NULL;
    }
    w->scratch = mpeg2_work_alloc(dec, 64);
    w->blk = mpeg2_work_alloc(dec, 64 * sizeof(int16_t));
    w->done = mpeg2_work_alloc(dec, dec->mb_w);
    if (!w->bits_buf || !w->y || !w->u || !w->v || !w->ref_y || !w->ref_u ||
        !w->ref_v || !w->scratch || !w->blk || !w->done) {
        return NULL;
    }
    memset(w->blk, 0, 64 * sizeof(int16_t));
    return w;
}

static bool setup_workers(mpeg2_dec_t *dec) {
    dec->work_used = dec->work_tables_end;
    dec->worker_count = 0;
    dec->workers[0] = setup_worker(dec);
    if (!dec->workers[0]) return false;
    dec->worker_count = 1;
    if (dec->threaded) {
        const size_t mark = dec->work_used;
        dec->workers[1] = setup_worker(dec);
        if (dec->workers[1]) {
            dec->worker_count = 2;
        } else {
            dec->work_used = mark;
        }
    }
    return true;
}

static bool same_layout(const seq_t *a, const seq_t *b) {
    return a->width == b->width && a->height == b->height && a->progressive == b->progressive;
}

static bool activate_layout(mpeg2_dec_t *dec) {
    const seq_t *seq = &dec->seq;
    if (seq->chroma_format != 1) {
        dec->error = "only 4:2:0 MPEG-2 is supported";
        return false;
    }
    if (dec->have_layout && dec->pool_size && same_layout(&dec->layout, seq)) {
        dec->layout = *seq;
        dec->info.matrix_coefficients = seq->matrix;
        dec->info.profile_and_level = seq->profile_level;
        return true;
    }
    const uint32_t mb_w = (seq->width + 15u) / 16;
    const uint32_t mb_h = seq->progressive ? (seq->height + 15u) / 16 : 2 * ((seq->height + 31u) / 32);
    const uint32_t w = mb_w * 16, h = mb_h * 16;
    if ((dec->config.max_mbs && mb_w * mb_h > dec->config.max_mbs) ||
        (dec->config.max_side && (w > dec->config.max_side || h > dec->config.max_side)) ||
        mb_h > MAX_ROWS || mb_w > MAX_ROWS * 2) {
        dec->error = "video is larger than this player supports";
        return false;
    }
    if (frames_held(dec)) {
        dec->error = "video format changed mid-stream";
        return false;
    }
    release_frames(dec);
    dec->have_layout = false;
    dec->mb_w = (uint16_t)mb_w;
    dec->mb_h = (uint16_t)mb_h;
    dec->width = (uint16_t)w;
    dec->height = (uint16_t)h;
    dec->packed_stride = w * 3 / 2;
    dec->frame_bytes = (size_t)dec->packed_stride * h;
    dec->ystride = w + ROW_MARGIN;
    dec->cstride = w / 2 + ROW_MARGIN;
    if (!setup_workers(dec)) {
        dec->error = "video is too wide for the decoder's work memory";
        return false;
    }

    uint8_t held = dec->config.held_pictures;
    if (held > MAX_HELD) held = MAX_HELD;
    size_t pool = (size_t)3 + held + 1;
    if (dec->config.frame_budget_bytes) {
        size_t room = dec->config.frame_budget_bytes / dec->frame_bytes;
        if (room < (size_t)3 + held) room = (size_t)3 + held;
        if (pool > room) pool = room;
    }
    dec->frame_block = dec->config.alloc(dec->config.ctx, dec->frame_bytes * pool + FRAME_SLACK);
    if (!dec->frame_block) {
        dec->error = "not enough memory for the reference frames";
        return false;
    }
    dec->pool_size = (uint8_t)pool;
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        dec->frames[i].data = dec->frame_block + dec->frame_bytes * i;
    }

    mpeg2_dec_stream_info_t *info = &dec->info;
    memset(info, 0, sizeof(*info));
    info->coded_width = (uint16_t)w;
    info->coded_height = (uint16_t)h;
    info->width = seq->width;
    info->height = seq->height;
    info->matrix_coefficients = seq->matrix;
    info->profile_and_level = seq->profile_level;
    dec->layout = *seq;
    dec->have_layout = true;
    dec->need_keyframe = true;
    return true;
}

static void queue_output(mpeg2_dec_t *dec, frame_t *f) {
    f->waiting = false;
    f->queued = true;
    dec->outq[(dec->outq_head + dec->outq_count) % MAX_POOL] = (uint8_t)(f - dec->frames);
    dec->outq_count++;
    dec->have_out = true;
    dec->out_gop = f->gop;
    dec->out_tr = f->tr;
}

static bool next_in_order(const mpeg2_dec_t *dec, const frame_t *f) {
    if (dec->layout.low_delay) return true;
    if (dec->have_out && f->gop == dec->out_gop) return f->tr == ((dec->out_tr + 1) & 1023);
    return f->tr == 0;
}

static void release_anchor_if_next(mpeg2_dec_t *dec) {
    frame_t *f = dec->new_ref;
    if (f && f->waiting && next_in_order(dec, f)) queue_output(dec, f);
}

static void run_rows(mpeg2_dec_t *dec, worker_t *w) {
    for (;;) {
        const int row = atomic_fetch_add(&dec->next_row, 1);
        if (row >= dec->mb_h) break;
        mpeg2_decode_row(w, row);
    }
}

static void worker_main(void *arg) {
    mpeg2_dec_t *dec = arg;
    const vdec_threads_t *t = dec->config.threads;
    vdec_k_prepare();
    for (;;) {
        t->sem_take(t->ctx, dec->sem_work);
        if (dec->stopping) break;
        if (dec->worker_count > 1) run_rows(dec, dec->workers[1]);
        t->sem_give(t->ctx, dec->sem_done);
    }
    t->sem_give(t->ctx, dec->sem_done);
}

static void build_rows(mpeg2_dec_t *dec) {
    uint16_t s = 0;
    for (uint16_t r = 0; r <= dec->mb_h; r++) {
        while (s < dec->slice_count && dec->slices[s].row < r) s++;
        dec->row_first[r] = s;
    }
    dec->row_first[dec->mb_h] = dec->slice_count;
}

static mpeg2_dec_result_t finish_picture(mpeg2_dec_t *dec) {
    dec->in_picture = false;
    frame_t *f = dec->cur;
    build_rows(dec);
    atomic_store(&dec->next_row, 0);
    atomic_store(&dec->unsupported, false);
    for (uint8_t i = 0; i < dec->worker_count; i++) dec->workers[i]->concealed = false;
    if (dec->worker_count > 1) {
        const vdec_threads_t *t = dec->config.threads;
        t->sem_give(t->ctx, dec->sem_work);
        run_rows(dec, dec->workers[0]);
        uint32_t wait_start = dec->config.clock ? dec->config.clock() : 0;
        t->sem_take(t->ctx, dec->sem_done);
#ifdef MPEG2_DEC_PROFILE
        if (dec->config.clock) dec->workers[0]->prof[MPEG2_PROF_WAIT] += dec->config.clock() - wait_start;
#else
        (void)wait_start;
#endif
    } else {
        run_rows(dec, dec->workers[0]);
    }
    dec->cur = NULL;
    if (atomic_load(&dec->unsupported)) {
        dec->error = "interlaced MPEG-2 is not supported";
        return MPEG2_DEC_UNSUPPORTED;
    }

    bool concealed = dec->pic_error;
    for (uint8_t i = 0; i < dec->worker_count; i++) concealed |= dec->workers[i]->concealed;
    f->concealed = concealed;
    f->tag = dec->cur_tag;
    f->type = dec->pic.type;
    f->tr = dec->pic.tr;
    f->gop = dec->gop;
    if (dec->pic.type == MPEG2_PICTURE_B) {
        queue_output(dec, f);
        release_anchor_if_next(dec);
    } else {
        if (dec->new_ref && dec->new_ref->waiting) queue_output(dec, dec->new_ref);
        dec->old_ref = dec->new_ref;
        dec->new_ref = f;
        f->waiting = true;
        dec->gop_anchors++;
        release_anchor_if_next(dec);
    }
    return concealed ? MPEG2_DEC_BAD_DATA : MPEG2_DEC_OK;
}

static mpeg2_dec_result_t start_picture(mpeg2_dec_t *dec) {
    const pic_t *pic = &dec->pic;
    if (!dec->have_layout) return MPEG2_DEC_NO_PICTURE;
    if (!pic->have_ext) {
        dec->error = "MPEG-1 video is not supported";
        return MPEG2_DEC_UNSUPPORTED;
    }
    if (pic->structure != 3) {
        dec->error = "interlaced MPEG-2 is not supported";
        return MPEG2_DEC_UNSUPPORTED;
    }
    if (dec->need_keyframe) {
        if (pic->type != MPEG2_PICTURE_I) return MPEG2_DEC_NO_PICTURE;
        dec->need_keyframe = false;
    }
    if (pic->type != MPEG2_PICTURE_I) {
        for (int s = 0; s < 2; s++) {
            for (int t = 0; t < 2; t++) {
                const uint8_t f = pic->f_code[s][t];
                const bool used = s == 0 || pic->type == MPEG2_PICTURE_B;
                if (used && (f < 1 || f > 9)) return MPEG2_DEC_BAD_DATA;
            }
        }
    }
    if (pic->type == MPEG2_PICTURE_P && !dec->new_ref) return MPEG2_DEC_NO_PICTURE;
    if (pic->type == MPEG2_PICTURE_B) {
        if (!dec->new_ref) return MPEG2_DEC_NO_PICTURE;
        const bool broken = dec->broken_link && dec->gop_anchors < 2;
        if (!dec->closed_gop && (!dec->old_ref || broken)) return MPEG2_DEC_NO_PICTURE;
    }

    frame_t *f = NULL;
    for (uint8_t i = 0; i < dec->pool_size && !f; i++) {
        if (frame_free(dec, &dec->frames[i])) f = &dec->frames[i];
    }
    if (!f) {
        dec->error = "no free frame buffer (too many pictures held)";
        return MPEG2_DEC_NO_FRAME;
    }
    f->waiting = false;
    f->queued = false;
    f->concealed = false;
    dec->cur = f;
    dec->conceal_ref = dec->new_ref;
    if (pic->type == MPEG2_PICTURE_P) {
        dec->pred_ref[0] = dec->new_ref;
        dec->pred_ref[1] = dec->new_ref;
    } else if (pic->type == MPEG2_PICTURE_B) {
        dec->pred_ref[0] = dec->old_ref ? dec->old_ref : dec->new_ref;
        dec->pred_ref[1] = dec->new_ref;
    } else {
        dec->pred_ref[0] = dec->pred_ref[1] = dec->new_ref;
    }
    dec->in_picture = true;
    dec->pic_error = false;
    dec->slice_count = 0;
    return MPEG2_DEC_OK;
}

static void add_slice(mpeg2_dec_t *dec, uint8_t code, const uint8_t *body, size_t len) {
    const uint16_t row = (uint16_t)(code - 1);
    if (row >= dec->mb_h || dec->slice_count >= MAX_SLICES ||
        (dec->slice_count && dec->slices[dec->slice_count - 1].row > row)) {
        dec->pic_error = true;
        return;
    }
    slice_ref_t *s = &dec->slices[dec->slice_count++];
    s->data = body;
    s->len = (uint32_t)len;
    s->row = row;
}

static bool is_fatal(mpeg2_dec_result_t r) {
    return r == MPEG2_DEC_UNSUPPORTED || r == MPEG2_DEC_NO_MEMORY || r == MPEG2_DEC_NO_FRAME;
}

static mpeg2_dec_result_t decode_packet(mpeg2_dec_t *dec, const uint8_t *data, size_t len) {
    vdec_k_prepare();
    mpeg2_dec_result_t result = MPEG2_DEC_NO_PICTURE;
    bool bad = false;
    bool skipping = false;
    bool pending = false;
    bool seq_needs_ext = false;
    unit_iter_t it = { data, data + len };
    uint8_t code;
    const uint8_t *body;
    size_t n;
    while (next_unit(&it, &code, &body, &n)) {
        if (code >= SC_SLICE_FIRST && code <= SC_SLICE_LAST) {
            if (skipping) continue;
            if (pending) {
                pending = false;
                const mpeg2_dec_result_t r = start_picture(dec);
                if (is_fatal(r)) return r;
                if (r != MPEG2_DEC_OK) {
                    if (r == MPEG2_DEC_BAD_DATA) bad = true;
                    skipping = true;
                    continue;
                }
            }
            if (dec->in_picture) add_slice(dec, code, body, n);
            continue;
        }
        if (code == SC_EXTENSION) {
            hbits_t h = { body, n, 0 };
            const bool was_seq = n > 0 && (body[0] >> 4) == EXT_SEQUENCE;
            if (!mpeg2_parse_extension(dec, &h)) {
                if (dec->error) return MPEG2_DEC_UNSUPPORTED;
                bad = true;
                continue;
            }
            if (was_seq && dec->have_seq) {
                seq_needs_ext = false;
                if (!activate_layout(dec)) return MPEG2_DEC_UNSUPPORTED;
            }
            continue;
        }
        if (seq_needs_ext) {
            dec->error = "MPEG-1 video is not supported";
            return MPEG2_DEC_UNSUPPORTED;
        }
        if (code == SC_USER_DATA) continue;
        if (dec->in_picture) {
            const mpeg2_dec_result_t r = finish_picture(dec);
            if (is_fatal(r)) return r;
            if (r == MPEG2_DEC_BAD_DATA) bad = true;
            result = MPEG2_DEC_OK;
        }
        pending = false;
        skipping = false;
        hbits_t h = { body, n, 0 };
        switch (code) {
        case SC_SEQUENCE: {
            seq_t seq;
            if (!mpeg2_parse_sequence(&h, &seq)) {
                bad = true;
                break;
            }
            dec->seq = seq;
            dec->have_seq = true;
            memcpy(dec->intra_q, seq.intra_q, 64);
            memcpy(dec->inter_q, seq.inter_q, 64);
            seq_needs_ext = true;
            break;
        }
        case SC_GOP:
            hbits_u(&h, 25);
            dec->closed_gop = hbits_u(&h, 1);
            dec->broken_link = hbits_u(&h, 1);
            dec->gop++;
            dec->gop_anchors = 0;
            break;
        case SC_PICTURE:
            dec->have_picture_header = mpeg2_parse_picture(&h, &dec->pic);
            if (dec->have_picture_header) {
                pending = true;
            } else {
                bad = true;
            }
            break;
        default:
            break;
        }
    }
    if (seq_needs_ext) {
        dec->error = "MPEG-1 video is not supported";
        return MPEG2_DEC_UNSUPPORTED;
    }
    if (dec->in_picture) {
        const mpeg2_dec_result_t r = finish_picture(dec);
        if (is_fatal(r)) return r;
        if (r == MPEG2_DEC_BAD_DATA) bad = true;
        result = MPEG2_DEC_OK;
    }
    if (result == MPEG2_DEC_OK) return bad ? MPEG2_DEC_BAD_DATA : MPEG2_DEC_OK;
    return bad ? MPEG2_DEC_BAD_DATA : MPEG2_DEC_NO_PICTURE;
}

mpeg2_dec_t *mpeg2_dec_create(const mpeg2_dec_config_t *config) {
    if (!config || !config->alloc || !config->free || !config->work) return NULL;
    if ((uintptr_t)config->work & (MPEG2_DEC_WORK_ALIGNMENT - 1)) return NULL;
    mpeg2_dec_t *dec = config->alloc(config->ctx, sizeof(*dec));
    if (!dec) return NULL;
    memset(dec, 0, sizeof(*dec));
    dec->config = *config;
    dec->work_base = config->work;
    if (!mpeg2_build_vlcs(dec)) {
        config->free(config->ctx, dec);
        return NULL;
    }
    dec->work_tables_end = dec->work_used;
    dec->need_keyframe = true;
    const vdec_threads_t *t = config->threads;
    if (t) {
        dec->sem_work = t->sem_create(t->ctx, 1, 0);
        dec->sem_done = t->sem_create(t->ctx, 1, 0);
        if (dec->sem_work && dec->sem_done && t->spawn(t->ctx, worker_main, dec)) {
            dec->threaded = true;
        } else {
            if (dec->sem_work) t->sem_delete(t->ctx, dec->sem_work);
            if (dec->sem_done) t->sem_delete(t->ctx, dec->sem_done);
            dec->sem_work = dec->sem_done = NULL;
        }
    }
    return dec;
}

void mpeg2_dec_destroy(mpeg2_dec_t *dec) {
    if (!dec) return;
    const vdec_threads_t *t = dec->config.threads;
    if (dec->threaded) {
        dec->stopping = true;
        t->sem_give(t->ctx, dec->sem_work);
        t->sem_take(t->ctx, dec->sem_done);
        t->sem_delete(t->ctx, dec->sem_work);
        t->sem_delete(t->ctx, dec->sem_done);
    }
    release_frames(dec);
    dec->config.free(dec->config.ctx, dec);
}

mpeg2_dec_result_t mpeg2_dec_decode(mpeg2_dec_t *dec, const uint8_t *data, size_t len, int64_t tag) {
    dec->error = NULL;
    dec->cur_tag = tag;
    const uint32_t start = dec->config.clock ? dec->config.clock() : 0;
    const mpeg2_dec_result_t r = decode_packet(dec, data, len);
#ifdef MPEG2_DEC_PROFILE
    if (dec->config.clock && dec->worker_count) {
        dec->workers[0]->prof[MPEG2_PROF_TOTAL] += dec->config.clock() - start;
    }
#else
    (void)start;
#endif
    if (is_fatal(r)) {
        dec->in_picture = false;
        dec->cur = NULL;
    if (atomic_load(&dec->unsupported)) {
        dec->error = "interlaced MPEG-2 is not supported";
        return MPEG2_DEC_UNSUPPORTED;
    }
    }
    return r;
}

bool mpeg2_dec_output(mpeg2_dec_t *dec, mpeg2_dec_picture_t *picture) {
    if (!dec->outq_count) return false;
    const uint8_t id = dec->outq[dec->outq_head];
    dec->outq_head = (uint8_t)((dec->outq_head + 1) % MAX_POOL);
    dec->outq_count--;
    frame_t *f = &dec->frames[id];
    f->queued = false;
    atomic_fetch_add(&f->holds, 1);
    picture->packed = f->data;
    picture->packed_bytes = dec->frame_bytes;
    picture->info = dec->info;
    picture->tag = f->tag;
    picture->id = id;
    picture->type = f->type;
    picture->concealed = f->concealed;
    return true;
}

void mpeg2_dec_drain(mpeg2_dec_t *dec) {
    if (dec->new_ref && dec->new_ref->waiting) queue_output(dec, dec->new_ref);
}

bool mpeg2_dec_stream_info(const mpeg2_dec_t *dec, mpeg2_dec_stream_info_t *info) {
    if (!dec->have_layout) return false;
    *info = dec->info;
    return true;
}

void mpeg2_dec_hold(mpeg2_dec_t *dec, uint8_t id) {
    if (id < dec->pool_size) atomic_fetch_add(&dec->frames[id].holds, 1);
}

void mpeg2_dec_release(mpeg2_dec_t *dec, uint8_t id) {
    if (id >= dec->pool_size) return;
    atomic_uchar *holds = &dec->frames[id].holds;
    unsigned char v = atomic_load(holds);
    while (v && !atomic_compare_exchange_weak(holds, &v, (unsigned char)(v - 1))) {}
}

void mpeg2_dec_flush(mpeg2_dec_t *dec) {
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        dec->frames[i].waiting = false;
        dec->frames[i].queued = false;
    }
    dec->outq_head = dec->outq_count = 0;
    dec->old_ref = dec->new_ref = dec->cur = NULL;
    dec->in_picture = false;
    dec->have_out = false;
    dec->need_keyframe = true;
    dec->closed_gop = false;
    dec->broken_link = false;
    dec->gop_anchors = 0;
}

const char *mpeg2_dec_error(const mpeg2_dec_t *dec) {
    return dec->error;
}

bool mpeg2_dec_take_profile(mpeg2_dec_t *dec, uint64_t out[MPEG2_PROF_COUNT]) {
#ifdef MPEG2_DEC_PROFILE
    memset(out, 0, sizeof(uint64_t) * MPEG2_PROF_COUNT);
    for (uint8_t i = 0; i < dec->worker_count; i++) {
        for (int k = 0; k < MPEG2_PROF_COUNT; k++) out[k] += dec->workers[i]->prof[k];
        memset(dec->workers[i]->prof, 0, sizeof(dec->workers[i]->prof));
    }
    return true;
#else
    (void)dec;
    (void)out;
    return false;
#endif
}

bool mpeg2_dec_probe(const uint8_t *data, size_t len, mpeg2_dec_stream_info_t *info,
                     const char **error) {
    unit_iter_t it = { data, data + len };
    uint8_t code;
    const uint8_t *body;
    size_t n;
    seq_t seq;
    bool have = false;
    while (next_unit(&it, &code, &body, &n)) {
        hbits_t h = { body, n, 0 };
        if (code == SC_SEQUENCE) {
            have = mpeg2_parse_sequence(&h, &seq);
            continue;
        }
        if (!have) continue;
        if (code == SC_EXTENSION && hbits_u(&h, 4) == EXT_SEQUENCE &&
            mpeg2_parse_sequence_extension(&h, &seq)) {
            if (seq.chroma_format != 1) {
                if (error) *error = "only 4:2:0 MPEG-2 is supported";
                return false;
            }
            memset(info, 0, sizeof(*info));
            info->width = seq.width;
            info->height = seq.height;
            info->coded_width = (uint16_t)((seq.width + 15u) & ~15u);
            info->coded_height = (uint16_t)(seq.progressive ? (seq.height + 15u) & ~15u
                                                            : (seq.height + 31u) & ~31u);
            info->profile_and_level = seq.profile_level;
            return true;
        }
        if (error) *error = "MPEG-1 video is not supported";
        return false;
    }
    if (error) *error = have ? "MPEG-1 video is not supported" : "no MPEG-2 sequence header found";
    return false;
}

bool mpeg2_dec_header(const uint8_t *data, size_t len, mpeg2_dec_header_t *header) {
    memset(header, 0, sizeof(*header));
    unit_iter_t it = { data, data + len };
    uint8_t code;
    const uint8_t *body;
    size_t n;
    while (next_unit(&it, &code, &body, &n)) {
        hbits_t h = { body, n, 0 };
        if (code == SC_GOP) {
            hbits_u(&h, 25);
            header->gop = true;
            header->closed_gop = hbits_u(&h, 1);
        } else if (code == SC_PICTURE) {
            header->temporal_reference = (uint16_t)hbits_u(&h, 10);
            header->type = (uint8_t)hbits_u(&h, 3);
            return !hbits_overrun(&h);
        } else if (code >= SC_SLICE_FIRST && code <= SC_SLICE_LAST) {
            return false;
        }
    }
    return false;
}

bool mpeg2_dec_droppable(const uint8_t *data, size_t len) {
    mpeg2_dec_header_t header;
    return mpeg2_dec_header(data, len, &header) && header.type == MPEG2_PICTURE_B;
}
