/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/demuxer.hpp"
#include "media/psram_allocator.hpp"

#include <string>

struct DirectoryEntry {
    PsramString name;
    bool directory;
    MediaKind kind;
};

/* Directories first, then files, each by name ignoring case; dot files are
   left out. False when the directory cannot be opened. */
bool media_directory_list(const std::string &path, PsramVector<DirectoryEntry> *entries);
