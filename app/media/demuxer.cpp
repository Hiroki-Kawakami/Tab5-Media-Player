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

bool starts_with_start_code(const uint8_t *data, std::size_t size) {
    return (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
           (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

bool append_nal_units(const uint8_t *data, std::size_t size, std::size_t *pos, std::size_t count,
                      std::vector<uint8_t> *out) {
    static constexpr uint8_t kStartCode[] = { 0, 0, 0, 1 };
    for (std::size_t i = 0; i < count; i++) {
        if (size - *pos < 2) return false;
        const std::size_t length = (std::size_t)(data[*pos] << 8 | data[*pos + 1]);
        *pos += 2;
        if (size - *pos < length) return false;
        out->insert(out->end(), kStartCode, kStartCode + sizeof(kStartCode));
        out->insert(out->end(), data + *pos, data + *pos + length);
        *pos += length;
    }
    return true;
}

const Format *format_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return nullptr;
    for (const Format &format : kFormats) {
        if (strcasecmp(dot, format.suffix) == 0) return &format;
    }
    return nullptr;
}

}

bool h264_config_to_annexb(const uint8_t *data, std::size_t size, std::vector<uint8_t> *annexb,
                           uint8_t *nal_length_size) {
    annexb->clear();
    *nal_length_size = 0;
    if (!data || size == 0) return false;
    if (starts_with_start_code(data, size)) {
        annexb->assign(data, data + size);
        return true;
    }
    if (size < 7 || data[0] != 1 || (data[4] & 0x03) == 2) return false;

    std::size_t pos = 6;
    if (!append_nal_units(data, size, &pos, data[5] & 0x1F, annexb) || pos >= size) {
        annexb->clear();
        return false;
    }
    const std::size_t pps_count = data[pos++];
    if (!append_nal_units(data, size, &pos, pps_count, annexb)) {
        annexb->clear();
        return false;
    }
    *nal_length_size = (uint8_t)((data[4] & 0x03) + 1);
    return true;
}

bool demuxer_supports(const char *name) {
    return format_of(name) != nullptr;
}

std::unique_ptr<Demuxer> demuxer_create(const std::string &path) {
    const Format *format = format_of(path.c_str());
    return format ? format->create() : nullptr;
}
