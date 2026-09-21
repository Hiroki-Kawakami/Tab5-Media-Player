/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "esp_heap_caps.h"
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

template <class T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() = default;
    template <class U> PsramAllocator(const PsramAllocator<U> &) {}

    T *allocate(std::size_t count) {
        void *memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM);
        if (!memory) abort();
        return static_cast<T *>(memory);
    }
    void deallocate(T *pointer, std::size_t) { heap_caps_free(pointer); }

    template <class U> bool operator==(const PsramAllocator<U> &) const { return true; }
    template <class U> bool operator!=(const PsramAllocator<U> &) const { return false; }
};

using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

template <class T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

template <class T>
using PsramDeque = std::deque<T, PsramAllocator<T>>;

template <class K, class V, class H = std::hash<K>>
using PsramMap = std::unordered_map<K, V, H, std::equal_to<K>,
                                    PsramAllocator<std::pair<const K, V>>>;

struct PsramStringHash {
    std::size_t operator()(const PsramString &value) const {
        std::size_t hash = 2166136261u;
        for (char c : value) {
            hash ^= (unsigned char)c;
            hash *= 16777619u;
        }
        return hash;
    }
};

template <class T, class... Args>
std::shared_ptr<T> psram_make_shared(Args &&...args) {
    return std::allocate_shared<T>(PsramAllocator<T>(), std::forward<Args>(args)...);
}
