/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct FileOperationStatus {
    std::size_t done = 0;
    std::size_t total = 0;
    uint64_t bytes_done = 0;
    uint64_t bytes_total = 0;
    std::string current;
    bool finished = false;
    bool cancelled = false;
    std::string failed_path;
    int error = 0;
};

struct FileCopy {
    std::string source;
    /* Must not exist yet. */
    std::string target;
};

/* Each runs on its own task, one operation at a time; false when one is
   already running or the task cannot be started. Directories go with their
   contents. */
bool file_operation_delete(std::vector<std::string> paths);
bool file_operation_copy(std::vector<FileCopy> copies);
FileOperationStatus file_operation_status();
/* Stops before the next file, or within the file being copied, whose partial
   copy is removed; what is already done stays done. */
void file_operation_cancel();
