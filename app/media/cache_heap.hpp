/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "multi_heap.h"

#include <cstddef>
#include <cstdint>

/* A heap of its own over one PSRAM region, so what is kept for a long time
   never splits the general heap. Safe to use from any task. */
class CacheHeap {
public:
    bool create(std::size_t bytes);
    /* Every block must have been released. */
    void destroy();
    bool ready() const { return heap_ != nullptr; }

    /* Null when the heap is full. */
    void *allocate(std::size_t bytes, std::size_t alignment = sizeof(void *));
    void release(void *pointer);
    bool contains(const void *pointer) const;

    std::size_t size() const { return size_; }
    std::size_t free_bytes() const;
    std::size_t largest_free() const;

private:
    uint8_t *region_ = nullptr;
    std::size_t size_ = 0;
    multi_heap_handle_t heap_ = nullptr;
};
