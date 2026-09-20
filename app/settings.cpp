/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "settings.hpp"
#include "bsp.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#ifndef NVS_KEY_NAME_MAX_SIZE
#define NVS_KEY_NAME_MAX_SIZE 16
#endif

namespace {

nvs_handle_t s_nvs;
bool s_nvs_open;

template <std::size_t N>
struct Key {
    static_assert(N <= NVS_KEY_NAME_MAX_SIZE, "NVS names are limited to 15 characters");

    char text[N];
    consteval Key(const char (&literal)[N]) {
        for (std::size_t i = 0; i < N; i++) text[i] = literal[i];
    }
};

constexpr Key kNamespace{"tab5mediaplayer"};

template <typename T>
struct Codec;

template <>
struct Codec<uint8_t> {
    static esp_err_t get(nvs_handle_t nvs, const char *key, uint8_t &value) {
        return nvs_get_u8(nvs, key, &value);
    }
    static esp_err_t set(nvs_handle_t nvs, const char *key, uint8_t value) {
        return nvs_set_u8(nvs, key, value);
    }
};

template <>
struct Codec<std::string> {
    static esp_err_t get(nvs_handle_t nvs, const char *key, std::string &value) {
        std::size_t size = 0;
        esp_err_t err = nvs_get_str(nvs, key, nullptr, &size);
        if (err != ESP_OK) return err;
        value.resize(size);
        err = nvs_get_str(nvs, key, value.data(), &size);
        value.resize(err == ESP_OK && size > 0 ? size - 1 : 0);
        return err;
    }
    static esp_err_t set(nvs_handle_t nvs, const char *key, const std::string &value) {
        return nvs_set_str(nvs, key, value.c_str());
    }
};

template <typename F>
struct SanitizeInput {
    using type = void;
};
template <typename R, typename A>
struct SanitizeInput<R (*)(A)> {
    using type = A;
};

template <Key K, typename T, auto Sanitize = nullptr>
class Setting {
    static constexpr bool kSanitized = !std::is_null_pointer_v<decltype(Sanitize)>;

public:
    using Input = std::conditional_t<kSanitized, typename SanitizeInput<decltype(Sanitize)>::type, T>;

    constexpr explicit Setting(T value) : value_(std::move(value)) {}

    const T &get() const { return value_; }

    bool set(Input value) {
        T next = sanitize(std::move(value));
        if (next == value_) return false;
        value_ = std::move(next);
        modified_ = true;
        return true;
    }

    void load(nvs_handle_t nvs) {
        T value{};
        if (Codec<T>::get(nvs, K.text, value) != ESP_OK) return;
        value_ = sanitize(std::move(value));
    }

    bool commit(nvs_handle_t nvs) {
        if (!modified_ || Codec<T>::set(nvs, K.text, value_) != ESP_OK) return false;
        modified_ = false;
        return true;
    }

private:
    static T sanitize(Input value) {
        if constexpr (kSanitized) {
            return Sanitize(std::move(value));
        } else {
            return value;
        }
    }

    T value_;
    bool modified_ = false;
};

Setting<"brightness", uint8_t, +[](int percent) -> uint8_t {
    return std::clamp(percent, kMinDisplayBrightness, 100);
}> s_display_brightness{80};

template <typename Fn>
void for_each_setting(Fn &&fn) {
    fn(s_display_brightness);
}

}  // namespace

void settings_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_nvs_open = err == ESP_OK && nvs_open(kNamespace.text, NVS_READWRITE, &s_nvs) == ESP_OK;
    if (s_nvs_open) for_each_setting([](auto &setting) { setting.load(s_nvs); });

    bsp_display_set_brightness(s_display_brightness.get());
}

void settings_commit() {
    if (!s_nvs_open) return;
    bool written = false;
    for_each_setting([&written](auto &setting) { written |= setting.commit(s_nvs); });
    if (written) nvs_commit(s_nvs);
}

int settings_display_brightness() {
    return s_display_brightness.get();
}

void settings_set_display_brightness(int percent) {
    if (!s_display_brightness.set(percent)) return;
    bsp_display_set_brightness(s_display_brightness.get());
}
