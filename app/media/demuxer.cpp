/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include <cstring>
#include <strings.h>

std::unique_ptr<Demuxer> avi_demuxer_create();

static constexpr const char *kAviSuffix = ".avi";

static const char *extension_of(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot : "";
}

bool demuxer_supports(const char *name) {
    return strcasecmp(extension_of(name), kAviSuffix) == 0;
}

std::unique_ptr<Demuxer> demuxer_create(const std::string &path) {
    if (strcasecmp(extension_of(path.c_str()), kAviSuffix) == 0) return avi_demuxer_create();
    return nullptr;
}
