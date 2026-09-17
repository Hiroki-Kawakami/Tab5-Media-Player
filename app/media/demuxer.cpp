/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include <cstring>
#include <strings.h>

std::unique_ptr<Demuxer> avi_demuxer_create();
std::unique_ptr<Demuxer> mkv_demuxer_create();

namespace {

struct Format {
    const char *suffix;
    std::unique_ptr<Demuxer> (*create)();
};

constexpr Format kFormats[] = {
    { ".avi", avi_demuxer_create },
    { ".mkv", mkv_demuxer_create },
};

const Format *format_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return nullptr;
    for (const Format &format : kFormats) {
        if (strcasecmp(dot, format.suffix) == 0) return &format;
    }
    return nullptr;
}

}

bool demuxer_supports(const char *name) {
    return format_of(name) != nullptr;
}

std::unique_ptr<Demuxer> demuxer_create(const std::string &path) {
    const Format *format = format_of(path.c_str());
    return format ? format->create() : nullptr;
}
