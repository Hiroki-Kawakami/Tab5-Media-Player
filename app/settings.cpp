/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "settings.hpp"
#include "lvgl.hpp"
#include "media_player.hpp"
#include "nvs_flash.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

Setting<"colordepth", uint8_t, +[](int bits) -> uint8_t {
    return bits == 24 ? 24 : 16;
}> s_display_color_depth{16};

constexpr auto clamp_volume = +[](int percent) -> uint8_t {
    return std::clamp(percent, 0, 100);
};

Setting<"spkvolume", uint8_t, clamp_volume> s_speaker_volume{kDefaultSpeakerVolume};
Setting<"hpvolume", uint8_t, clamp_volume> s_headphone_volume{kDefaultHeadphoneVolume};

Setting<"equalizer", uint8_t, +[](bool enabled) -> uint8_t {
    return enabled ? 1 : 0;
}> s_equalizer{1};

template <typename Fn>
void for_each_setting(Fn &&fn) {
    fn(s_display_brightness);
    fn(s_display_color_depth);
    fn(s_speaker_volume);
    fn(s_headphone_volume);
    fn(s_equalizer);
}

std::atomic<bool> s_headphone;

struct VolumeObserver {
    lv_obj_t *owner;
    std::function<void(int)> on_change;
};
std::vector<VolumeObserver> s_volume_observers;

void headphone_changed(bool inserted, void *) {
    s_headphone = inserted;
    bsp_audio_set_volume(settings_volume());
    lv_lock();
    lv_async_call([] {
        for (auto &observer : s_volume_observers) observer.on_change(settings_volume());
    });
    lv_unlock();
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
}

void settings_apply() {
    bsp_display_set_brightness(s_display_brightness.get());
    s_headphone = bsp_audio_headphone_inserted();
    bsp_audio_set_volume(settings_volume());
    bsp_audio_set_eq_enabled(s_equalizer.get() != 0);
    bsp_audio_set_headphone_callback(headphone_changed, nullptr);
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

bsp_pixel_format_t settings_display_pixel_format() {
    return s_display_color_depth.get() == 24 ? BSP_PIXEL_FORMAT_RGB888
                                             : BSP_PIXEL_FORMAT_RGB565;
}

esp_err_t settings_set_display_pixel_format(bsp_pixel_format_t format) {
    if (format == settings_display_pixel_format()) return ESP_OK;
    esp_err_t err = media_player_set_display_pixel_format(format);
    if (err != ESP_OK) return err;
    s_display_color_depth.set((int)bsp_pixel_format_bytes(format) * 8);
    return ESP_OK;
}

bool settings_volume_is_headphone() {
    return s_headphone;
}

int settings_volume() {
    return s_headphone ? s_headphone_volume.get() : s_speaker_volume.get();
}

void settings_set_volume(int percent) {
    const bool changed = s_headphone ? s_headphone_volume.set(percent)
                                     : s_speaker_volume.set(percent);
    if (!changed) return;
    bsp_audio_set_volume(settings_volume());
}

void settings_volume_observe(lv_obj_t *owner, std::function<void(int)> on_change) {
    s_volume_observers.push_back({owner, std::move(on_change)});
    // lv_obj_add_event_fn cannot carry LV_EVENT_DELETE: it frees its own closure
    // from an earlier callback on that same event.
    lv_obj_add_event_cb(owner, [](lv_event_t *event) {
        auto owner = (lv_obj_t *)lv_event_get_user_data(event);
        for (auto it = s_volume_observers.begin(); it != s_volume_observers.end(); ++it) {
            if (it->owner != owner) continue;
            s_volume_observers.erase(it);
            return;
        }
    }, LV_EVENT_DELETE, owner);
}

bool settings_equalizer_enabled() {
    return s_equalizer.get() != 0;
}

void settings_set_equalizer_enabled(bool enabled) {
    if (!s_equalizer.set(enabled)) return;
    bsp_audio_set_eq_enabled(enabled);
}
