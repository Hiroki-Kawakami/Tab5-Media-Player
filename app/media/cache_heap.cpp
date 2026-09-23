/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "cache_heap.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static constexpr std::size_t kRegionAlignment = 64;

/* multi_heap takes no lock of its own, and one mutex for every heap costs less
   internal RAM than one each. */
static SemaphoreHandle_t s_lock;

namespace {

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

}

bool CacheHeap::create(std::size_t bytes) {
    if (heap_) return true;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    region_ = static_cast<uint8_t *>(heap_caps_aligned_alloc(kRegionAlignment, bytes,
                                                             MALLOC_CAP_SPIRAM));
    if (!region_) return false;
    heap_ = multi_heap_register(region_, bytes);
    if (!heap_) {
        heap_caps_free(region_);
        region_ = nullptr;
        return false;
    }
    size_ = bytes;
    return true;
}

void CacheHeap::destroy() {
    if (!heap_) return;
    Lock lock;
    heap_ = nullptr;
    heap_caps_free(region_);
    region_ = nullptr;
    size_ = 0;
}

void *CacheHeap::allocate(std::size_t bytes, std::size_t alignment) {
    if (!heap_) return nullptr;
    Lock lock;
    return multi_heap_aligned_alloc(heap_, bytes, alignment);
}

void CacheHeap::release(void *pointer) {
    if (!heap_ || !pointer) return;
    Lock lock;
    multi_heap_free(heap_, pointer);
}

bool CacheHeap::contains(const void *pointer) const {
    const auto *at = static_cast<const uint8_t *>(pointer);
    return region_ && at >= region_ && at < region_ + size_;
}

std::size_t CacheHeap::free_bytes() const {
    if (!heap_) return 0;
    Lock lock;
    return multi_heap_free_size(heap_);
}

std::size_t CacheHeap::largest_free() const {
    if (!heap_) return 0;
    Lock lock;
    multi_heap_info_t info;
    multi_heap_get_info(heap_, &info);
    return info.largest_free_block;
}
