/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_types.hpp"
#include "medf_buffer.h"
#include <string>

/* `want_cover` false leaves `out->cover` empty but still fills `out->cover_at`
   when the container said where the picture is. */
bool media_probe(const std::string &path, const medf_arena_t &arena, bool want_cover,
                 MediaSummary *out, std::string *error);

/* Reads just the picture at `at`, for a file already probed without it. */
CoverArt media_probe_cover(const std::string &path, const medf_arena_t &arena,
                           const CoverLocation &at);

/* A frame of the video that decodes on its own, for a file without cover art.
   An MJPEG frame is a JPEG left in the file at `at`, and `bytes` only holds its
   head; anything else is read whole into `bytes`. */
struct VideoKeyframe {
    CodecId codec = CodecId::None;
    bsp_rotation_t rotation = BSP_ROTATION_0;
    CoverLocation at;
    std::shared_ptr<const CoverBytes> bytes;
    PsramVector<uint8_t> config;
    uint8_t nal_length_size = 0;
};

bool media_probe_keyframe(const std::string &path, const medf_arena_t &arena,
                          VideoKeyframe *out);
