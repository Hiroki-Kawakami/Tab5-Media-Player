/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_player.hpp"
#include "audio/audio_output.hpp"
#include "display_manager.hpp"
#include "lvgl.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <mutex>
#include "bench/h264_bench.hpp"
#include "bench/mpeg2_bench.hpp"
#include "media/media_cache.hpp"
#include "playback/player.hpp"
#include "screen_manager.hpp"
#include "screens/home_screen.hpp"
#include "screens/audio_player_screen.hpp"
#include "screens/image_viewer/bgm_picker_screen.hpp"
#include "screens/image_viewer_screen.hpp"
#include "screens/video_input_screen.hpp"
#include "screens/video_player_screen.hpp"
#include "settings.hpp"
#include "ui_font.hpp"
#include "ui_orientation.hpp"
#include "usb_host.hpp"
#include "usb_host_msc.hpp"
#include "usb_host_uac.hpp"
#include "usb_host_uvc.hpp"
#ifndef ESP_PLATFORM
#include "wifi_sim.hpp"
#endif

static const char *TAG = "media_player";

static constexpr std::size_t kMediaArenaBytes = 4 * 1024 * 1024;
static constexpr std::size_t kProbeArenaBytes = 512 * 1024;

alignas(64) static uint8_t s_shared_sram[kSharedSramBytes];
static lv_display_t *s_main;
static std::weak_ptr<HomeScreen> s_home;
static std::mutex s_usb_lock;
static std::shared_ptr<usb_host::MscDevice> s_usb_drive;
static std::shared_ptr<usb_host::UvcDevice> s_camera;
static std::shared_ptr<usb_host::UacCaptureDevice> s_capture_audio;

static SharedSram shared_sram() {
    const std::size_t half = kSharedSramBytes / 2;
    return { s_shared_sram, kSharedSramBytes, { s_shared_sram, s_shared_sram + half }, half };
}

static esp_err_t display_init() {
    lvgl_port_cfg_t config = {
        .task_priority     = 4,
        .task_stack        = 8192,
        .task_affinity     = 1,
        .task_max_sleep_ms = 500,
        .task_stack_caps   = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms   = 5,
    };
    esp_err_t err = lvgl_port_init(&config);
    if (err != ESP_OK) return err;

    const SharedSram sram = shared_sram();
    DisplayManagerConfig display_config = {};
    display_config.render_mode = DisplayRenderMode::Partial;
    display_config.buffer.buffers[0] = sram.halves[0];
    display_config.buffer.buffers[1] = sram.halves[1];
    display_config.buffer.buffer_size = sram.half_bytes;
    return display_manager.create_display(display_config, &s_main);
}

esp_err_t media_player_mount_sd() {
    if (bsp_sd_is_mounted()) return ESP_OK;
    bsp_sd_mount_config_t config = {};
    config.psram_bounce_buffer = true;
    return bsp_sd_mount(kSdMountPoint, &config);
}

esp_err_t media_player_mount_usb() {
    if (usb_host::mounted(kUsbMountPoint)) return ESP_OK;
    usb_host::unmount(kUsbMountPoint);
    std::shared_ptr<usb_host::MscDevice> drive;
    {
        std::lock_guard<std::mutex> guard(s_usb_lock);
        drive = s_usb_drive;
    }
    if (!drive) return ESP_ERR_NOT_FOUND;
    return usb_host::mount(drive, kUsbMountPoint);
}

bool media_player_usb_connected() {
    std::lock_guard<std::mutex> guard(s_usb_lock);
    return s_usb_drive != nullptr;
}

std::shared_ptr<usb_host::UvcDevice> media_player_camera() {
    std::lock_guard<std::mutex> guard(s_usb_lock);
    return s_camera;
}

std::shared_ptr<usb_host::UacCaptureDevice> media_player_capture_audio() {
    std::lock_guard<std::mutex> guard(s_usb_lock);
    return s_capture_audio;
}

SharedSram media_player_acquire_sram() {
    display_manager.set_visible(s_main, false);
    bsp_display_wait_draw();
    return shared_sram();
}

void media_player_release_sram() {
    display_manager.set_visible(s_main, true);
}

esp_err_t media_player_set_display_pixel_format(bsp_pixel_format_t format) {
    display_manager.set_visible(s_main, false);
    bsp_display_wait_draw();
    const esp_err_t err = bsp_display_reconfigure(format, 0);
    display_manager.set_color_format(s_main);
    display_manager.set_visible(s_main, true);
    return err;
}

void app_entry() {
    settings_init();

    bsp_config_t bsp_config = {};
    bsp_config.display.fb_num = 3;
    bsp_config.display.pixel_format = settings_display_pixel_format();
    bsp_config.dispatch.task_priority = 6;
    bsp_config.dispatch.task_affinity = 1;
    bsp_config.audio.speaker_mode = BSP_AUDIO_SPEAKER_MODE_AUTO;
    bsp_init(&bsp_config);
    settings_apply();
    audio_output_init();

    esp_err_t err = display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init: %s", esp_err_to_name(err));
        return;
    }
    ui_font_init();
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "internal heap free after display init: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
    media_arena_t arena = {};
    arena.data = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        MB_ARENA_ALIGNMENT, kMediaArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (arena.data) {
        arena.size = kMediaArenaBytes;
    } else {
        ESP_LOGE(TAG, "no memory for the media arena");
    }
    player_start(arena);

    media_arena_t probe_arena = {};
    probe_arena.data = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        MB_ARENA_ALIGNMENT, kProbeArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (probe_arena.data) {
        probe_arena.size = kProbeArenaBytes;
        probe_arena.direct = true;
        media_cache_init(probe_arena);
    } else {
        ESP_LOGE(TAG, "no memory for the probe arena");
    }
    media_cache_register_harness();
    h264_bench_register();
    mpeg2_bench_register();
#ifndef ESP_PLATFORM
    wifi::sim::register_harness_commands();
#endif

    err = bsp_power_set_switch(BSP_POWER_SWITCH_USB5V, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "USB 5V: %s", esp_err_to_name(err));
    }
    usb_host::Callbacks usb_callbacks;
    usb_callbacks.msc_connected = [](std::shared_ptr<usb_host::MscDevice> device) {
        {
            std::lock_guard<std::mutex> guard(s_usb_lock);
            if (s_usb_drive) return;
            s_usb_drive = std::move(device);
        }
        lv_lock();
        lv_async_call([] {
            BgmPickerScreen::refresh_storages();
            if (auto home = s_home.lock()) home->refresh_menu();
        });
        lv_unlock();
    };
    usb_callbacks.msc_disconnected = [](const std::shared_ptr<usb_host::MscDevice> &device) {
        {
            std::lock_guard<std::mutex> guard(s_usb_lock);
            if (s_usb_drive != device) return;
            s_usb_drive.reset();
        }
        player_eject(kUsbMountPoint);
        lv_lock();
        lv_async_call([] {
            media_cache_forget(kUsbMountPoint);
            AudioPlayerScreen::eject(kUsbMountPoint);
            VideoPlayerScreen::eject(kUsbMountPoint);
            BgmPickerScreen::eject(kUsbMountPoint);
            ImageViewerScreen::eject(kUsbMountPoint);
            BgmPickerScreen::refresh_storages();
            if (auto home = s_home.lock()) {
                home->eject(kUsbMountPoint);
                home->refresh_menu();
            }
        });
        lv_unlock();
    };
    usb_callbacks.uvc_connected = [](std::shared_ptr<usb_host::UvcDevice> device) {
        {
            std::lock_guard<std::mutex> guard(s_usb_lock);
            if (s_camera) return;
            s_camera = std::move(device);
        }
        lv_lock();
        lv_async_call([] {
            if (auto home = s_home.lock()) home->refresh_menu();
        });
        lv_unlock();
    };
    usb_callbacks.uvc_disconnected = [](const std::shared_ptr<usb_host::UvcDevice> &device) {
        {
            std::lock_guard<std::mutex> guard(s_usb_lock);
            if (s_camera != device) return;
            s_camera.reset();
        }
        lv_lock();
        lv_async_call([] {
            VideoInputScreen::unplugged();
            if (auto home = s_home.lock()) home->refresh_menu();
        });
        lv_unlock();
    };
    usb_callbacks.uac_capture_connected = [](std::shared_ptr<usb_host::UacCaptureDevice> device) {
        std::lock_guard<std::mutex> guard(s_usb_lock);
        if (!s_capture_audio) s_capture_audio = std::move(device);
    };
    usb_callbacks.uac_capture_disconnected =
        [](const std::shared_ptr<usb_host::UacCaptureDevice> &device) {
            std::lock_guard<std::mutex> guard(s_usb_lock);
            if (s_capture_audio == device) s_capture_audio.reset();
        };
    usb_callbacks.uac_connected = audio_output_usb_connected;
    usb_callbacks.uac_disconnected = audio_output_usb_disconnected;
    err = usb_host::install(std::move(usb_callbacks));
    if (err != ESP_OK) ESP_LOGE(TAG, "usb host install: %s", esp_err_to_name(err));
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "internal heap free after usb init: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif

    lv_async_call([] {
        media_cache_start();
        ui_orientation_start(s_main, settings_rotation_locked(), settings_rotation());
        auto home = std::make_shared<HomeScreen>();
        s_home = home;
        screen_manager.load(home);
    });
}
