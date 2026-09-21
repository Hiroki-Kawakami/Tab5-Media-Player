/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_probe.hpp"
#include "demuxer.hpp"
#include "media/psram_allocator.hpp"

bool media_probe(const std::string &path, const media_arena_t &arena, bool want_cover,
                 MediaSummary *out, std::string *error) {
    *out = {};
    error->clear();

    std::unique_ptr<Demuxer> demuxer = demuxer_create(path);
    if (!demuxer) {
        *error = "unsupported file";
        return false;
    }
    if (!demuxer->open(path, arena, want_cover)) {
        *error = demuxer->error();
        if (error->empty()) *error = "cannot open";
        demuxer->close();
        return false;
    }

    *out = media_summary_make(path, demuxer->info(), demuxer->bytes(), {});
    demuxer->close();
    return true;
}

CoverArt media_probe_cover(const std::string &path, const media_arena_t &arena,
                           const CoverLocation &at) {
    CoverArt cover;
    if (!at) return cover;

    media_buffer_t *reader = mb_open(path.c_str(), &arena);
    if (!reader) return cover;

    void *owner = nullptr;
    uint8_t *data = mb_read_alloc(reader, (off_t)at.offset, at.size, &owner);
    mb_close(reader);
    if (!data) return cover;

    cover.data = psram_make_shared<CoverBytes>((uint8_t *)owner, data, at.size);
    cover.format = at.size >= 4 && data[0] == 0x89 && data[1] == 'P' ? CoverFormat::Png
                                                                     : CoverFormat::Jpeg;
    return cover;
}
