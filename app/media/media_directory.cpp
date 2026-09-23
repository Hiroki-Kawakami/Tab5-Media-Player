/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_directory.hpp"

#include <algorithm>
#include <dirent.h>
#include <strings.h>

bool media_directory_list(const std::string &path, PsramVector<DirectoryEntry> *entries) {
    entries->clear();
    DIR *dir = opendir(path.c_str());
    if (!dir) return false;
    while (struct dirent *ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        const bool directory = ent->d_type == DT_DIR;
        const MediaKind kind = directory ? MediaKind::None : demuxer_media_kind(ent->d_name);
        entries->push_back({ PsramString(ent->d_name), directory, kind });
    }
    closedir(dir);

    std::sort(entries->begin(), entries->end(), [](const DirectoryEntry &a, const DirectoryEntry &b) {
        if (a.directory != b.directory) return a.directory;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    entries->shrink_to_fit();
    return true;
}
