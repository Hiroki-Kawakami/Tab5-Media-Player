/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_types.hpp"
#include "media_buffer.h"
#include <string>

bool media_probe(const std::string &path, const media_arena_t &arena, MediaSummary *out,
                 std::string *error);
