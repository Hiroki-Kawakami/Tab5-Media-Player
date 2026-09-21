/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_types.hpp"
#include "media_buffer.h"
#include <string>

/* `want_cover` false leaves `out->cover` empty but still fills `out->cover_at`
   when the container said where the picture is. */
bool media_probe(const std::string &path, const media_arena_t &arena, bool want_cover,
                 MediaSummary *out, std::string *error);

/* Reads just the picture at `at`, for a file already probed without it. */
CoverArt media_probe_cover(const std::string &path, const media_arena_t &arena,
                           const CoverLocation &at);
