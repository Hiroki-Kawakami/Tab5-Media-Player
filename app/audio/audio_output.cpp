/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_output.hpp"
#include "usb_audio_output.hpp"
#include "bsp.h"
#include "lvgl.hpp"
#include "media/psram_allocator.hpp"
#include "settings.hpp"
#include <atomic>
#include <cmath>
#include <mutex>
#include <new>
#include <utility>

namespace {

struct VolumeObserver {
    lv_obj_t *owner;
    std::function<void(int)> on_change;
};

struct State {
    std::mutex stream_lock;
    bool open = false;
    uint32_t rate = 0;
    uint8_t bits = 0;
    uint8_t channels = 0;
    AudioContent content = AudioContent::Music;
    std::unique_ptr<UsbAudioOutput> usb_stream;
    std::atomic<float> usb_gain{1.0f};

    std::mutex route_lock;
    std::shared_ptr<usb_host::UacDevice> usb;
    bool headphone = false;
    bool mute = false;
    int transient = -1;

    PsramVector<VolumeObserver> observers;
};

State *s_state;

AudioRoute route_locked(const State &state) {
    if (state.usb) return AudioRoute::Usb;
    return state.headphone ? AudioRoute::Headphone : AudioRoute::Speaker;
}

float usb_volume_db(const usb_host::UacDevice &device, int percent) {
    const float min = device.volume_min_db();
    return min + (device.volume_max_db() - min) * percent / 100.0f;
}

float board_curve_gain(int percent) {
    if (percent <= 0) return 0.0f;
    return powf(10.0f, (percent - 100) * 0.4f / 20.0f);
}

void apply_locked(State &state) {
    const AudioRoute board_route = state.headphone ? AudioRoute::Headphone : AudioRoute::Speaker;
    int board = settings_route_volume(board_route);
    if (!state.usb && state.transient >= 0) board = state.transient;
    bsp_audio_set_volume(board);
    bsp_audio_set_mute(state.mute);
    if (!state.usb) return;

    const int usb = state.transient >= 0 ? state.transient : settings_route_volume(AudioRoute::Usb);
    const bool muted = state.mute || usb <= 0;
    state.usb->set_mute(muted);
    if (state.usb->has_volume()) {
        state.usb->set_volume_db(muted && !state.usb->has_mute() ? -INFINITY
                                                                : usb_volume_db(*state.usb, usb));
    } else {
        state.usb_gain = muted && !state.usb->has_mute() ? 0.0f : board_curve_gain(usb);
    }
}

void notify_observers() {
    lv_lock();
    lv_async_call([] {
        for (auto &observer : s_state->observers) observer.on_change(audio_output_volume());
    });
    lv_unlock();
}

void headphone_changed(bool inserted, void *) {
    {
        std::lock_guard<std::mutex> guard(s_state->route_lock);
        s_state->headphone = inserted;
        s_state->transient = -1;
        apply_locked(*s_state);
    }
    notify_observers();
}

}  // namespace

void audio_output_init() {
    if (s_state) return;
    void *memory = heap_caps_malloc(sizeof(State), MALLOC_CAP_SPIRAM);
    if (!memory) abort();
    s_state = new (memory) State();
    {
        std::lock_guard<std::mutex> guard(s_state->route_lock);
        s_state->headphone = bsp_audio_headphone_inserted();
        apply_locked(*s_state);
    }
    bsp_audio_set_headphone_callback(headphone_changed, nullptr);
}

AudioRoute audio_output_route() {
    std::lock_guard<std::mutex> guard(s_state->route_lock);
    return route_locked(*s_state);
}

const char *audio_route_name(AudioRoute route) {
    switch (route) {
    case AudioRoute::Headphone: return "Headphone";
    case AudioRoute::Usb: return "USB Audio";
    default: return "Speaker";
    }
}

int audio_output_volume() {
    return settings_route_volume(audio_output_route());
}

void audio_output_set_volume(int percent) {
    std::lock_guard<std::mutex> guard(s_state->route_lock);
    const bool changed = settings_set_route_volume(route_locked(*s_state), percent);
    if (!changed && s_state->transient < 0) return;
    s_state->transient = -1;
    apply_locked(*s_state);
}

void audio_output_apply_volume(int percent) {
    std::lock_guard<std::mutex> guard(s_state->route_lock);
    s_state->transient = percent;
    apply_locked(*s_state);
}

void audio_output_volume_observe(lv_obj_t *owner, std::function<void(int)> on_change) {
    s_state->observers.push_back({owner, std::move(on_change)});
    // lv_obj_add_event_fn cannot carry LV_EVENT_DELETE: it frees its own closure
    // from an earlier callback on that same event.
    lv_obj_add_event_cb(owner, [](lv_event_t *event) {
        auto owner = (lv_obj_t *)lv_event_get_user_data(event);
        auto &observers = s_state->observers;
        for (auto it = observers.begin(); it != observers.end(); ++it) {
            if (it->owner != owner) continue;
            observers.erase(it);
            return;
        }
    }, LV_EVENT_DELETE, owner);
}

void audio_output_set_mute(bool mute) {
    std::lock_guard<std::mutex> guard(s_state->route_lock);
    if (s_state->mute == mute) return;
    s_state->mute = mute;
    apply_locked(*s_state);
}

bool audio_output_get_mute() {
    std::lock_guard<std::mutex> guard(s_state->route_lock);
    return s_state->mute;
}

void audio_output_usb_connected(std::shared_ptr<usb_host::UacDevice> device) {
    {
        std::lock_guard<std::mutex> guard(s_state->route_lock);
        if (s_state->usb) return;
        s_state->usb = device;
        s_state->transient = -1;
        apply_locked(*s_state);
    }
    {
        std::lock_guard<std::mutex> guard(s_state->stream_lock);
        s_state->usb_stream = std::make_unique<UsbAudioOutput>(std::move(device), &s_state->usb_gain);
        if (s_state->open) {
            bsp_audio_close();
            s_state->usb_stream->open(s_state->rate, s_state->bits, s_state->channels,
                                      s_state->content);
        }
    }
    notify_observers();
}

void audio_output_usb_disconnected(const std::shared_ptr<usb_host::UacDevice> &device) {
    {
        std::lock_guard<std::mutex> guard(s_state->route_lock);
        if (s_state->usb != device) return;
        s_state->usb.reset();
        s_state->transient = -1;
        apply_locked(*s_state);
    }
    {
        std::lock_guard<std::mutex> guard(s_state->stream_lock);
        if (s_state->usb_stream && s_state->usb_stream->device() == device) {
            s_state->usb_stream.reset();
            if (s_state->open) bsp_audio_open(s_state->rate, s_state->bits, s_state->channels);
        }
    }
    notify_observers();
}

esp_err_t audio_output_open(uint32_t rate, uint8_t bits, uint8_t channels, AudioContent content) {
    std::lock_guard<std::mutex> guard(s_state->stream_lock);
    State &state = *s_state;
    if (state.open && state.rate == rate && state.bits == bits && state.channels == channels &&
        state.content == content) {
        return ESP_OK;
    }
    state.rate = rate;
    state.bits = bits;
    state.channels = channels;
    state.content = content;
    if (state.usb_stream) {
        state.usb_stream->open(rate, bits, channels, content);
        state.open = true;
        return ESP_OK;
    }
    const esp_err_t err = bsp_audio_open(rate, bits, channels);
    state.open = err == ESP_OK;
    return err;
}

void audio_output_close() {
    std::lock_guard<std::mutex> guard(s_state->stream_lock);
    if (!s_state->open) return;
    s_state->open = false;
    if (s_state->usb_stream) {
        s_state->usb_stream->close();
    } else {
        bsp_audio_close();
    }
}

esp_err_t audio_output_write(void *data, std::size_t len) {
    std::lock_guard<std::mutex> guard(s_state->stream_lock);
    if (!s_state->open) return ESP_ERR_INVALID_STATE;
    if (s_state->usb_stream) return s_state->usb_stream->write(data, len);
    return bsp_audio_write(data, len);
}
