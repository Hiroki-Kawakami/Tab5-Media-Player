/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_probe.hpp"
#include "demuxer.hpp"

bool media_probe(const std::string &path, const media_arena_t &arena, MediaSummary *out,
                 std::string *error) {
    *out = {};
    error->clear();

    std::unique_ptr<Demuxer> demuxer = demuxer_create(path);
    if (!demuxer) {
        *error = "unsupported file";
        return false;
    }
    if (!demuxer->open(path, arena)) {
        *error = demuxer->error();
        if (error->empty()) *error = "cannot open";
        demuxer->close();
        return false;
    }

    *out = media_summary_make(path, demuxer->info(), demuxer->bytes(), {});
    demuxer->close();
    return true;
}
