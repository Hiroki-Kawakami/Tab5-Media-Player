/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <string>
#include <vector>

struct FileOperationStatus {
    std::size_t done = 0;
    std::size_t total = 0;
    std::string current;
    bool finished = false;
    bool cancelled = false;
    std::string failed_path;
    int error = 0;
};

/* Runs on its own task, one operation at a time; false when one is already
   running or the task cannot be started. Directories go with their contents. */
bool file_operation_delete(std::vector<std::string> paths);
FileOperationStatus file_operation_status();
/* Stops before the next file; what is already gone stays gone. */
void file_operation_cancel();
