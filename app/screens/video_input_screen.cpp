/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "video_input_screen.hpp"
#include "audio/audio_output.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "playback/player.hpp"
#include "screens/media_controls.hpp"
#include "screens/video_player/settings_panel.hpp"
#include "video/video_presenter.hpp"
#include "ui_orientation.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "resources.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "video_input";

static constexpr uint16_t kCaptureWidth = 1280;
static constexpr uint16_t kCaptureHeight = 720;
static constexpr uint32_t kCaptureInterval = 333333;
static constexpr std::size_t kSlotBytes = 1024 * 1024;
static constexpr uint32_t kReceiveTimeoutMs = 100;
static constexpr uint32_t kFeedStackBytes = 4096;
static constexpr UBaseType_t kFeedPriority = 3;

static constexpr int32_t kBarHeight = 80;
static constexpr int32_t kPortraitPanelHeight = 640;
static constexpr int32_t kLandscapePanelWidth = 560;
static constexpr uint32_t kRefreshPeriodMs = 500;
static constexpr uint32_t kAutoHideMs = 4000;
static constexpr int32_t kBarPadding = 24;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kVolumeSlider = 280;
static constexpr int32_t kOverlayBufferLines = 32;

static constexpr uint32_t kBarColor = 0x101010;
static constexpr uint32_t kTrackColor = 0x404040;
static constexpr uint32_t kMessageColor = 0xffb74d;

static constexpr const char *kTitle = "Video Input";

/* Clockwise, so a rotation is a shift along the cycle. */
enum class Edge { Top, Right, Bottom, Left };

static VideoInputScreen *s_active;

static bool is_portrait(bsp_rotation_t rotation) {
    return rotation == BSP_ROTATION_0 || rotation == BSP_ROTATION_180;
}

static Edge panel_edge(Edge edge, bsp_rotation_t rotation) {
    return (Edge)(((int)edge - (int)rotation + 4) % 4);
}

static void add_inset(VideoInsets &insets, Edge edge, int32_t thickness) {
    switch (edge) {
    case Edge::Top:    insets.top = thickness; break;
    case Edge::Right:  insets.right = thickness; break;
    case Edge::Bottom: insets.bottom = thickness; break;
    default:           insets.left = thickness; break;
    }
}

static lv_obj_t *create_bar(lv_obj_t *parent, int32_t width, int32_t height, lv_align_t align) {
    lv_obj_t *bar = lv_container_create(parent, lv_color_hex(kBarColor));
    lv_obj_set_size(bar, width, height);
    lv_obj_align(bar, align, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

VideoInputScreen::VideoInputScreen(std::shared_ptr<usb_host::UvcDevice> camera)
    : camera_(std::move(camera)), title_(camera_->name().empty() ? kTitle : camera_->name()) {}

void VideoInputScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
}

VideoInsets VideoInputScreen::insets() const {
    VideoInsets insets;
    switch (mode_) {
    case UiMode::Bars:
        add_inset(insets, panel_edge(Edge::Top, rotation_), kBarHeight);
        add_inset(insets, panel_edge(Edge::Bottom, rotation_), kBarHeight);
        break;
    case UiMode::Settings:
        if (is_portrait(rotation_)) {
            add_inset(insets, panel_edge(Edge::Bottom, rotation_), kPortraitPanelHeight);
        } else {
            add_inset(insets, panel_edge(Edge::Right, rotation_), kLandscapePanelWidth);
        }
        break;
    default:
        break;
    }
    return insets;
}

bool VideoInputScreen::openOverlay() {
    DisplayManagerConfig config = {};
    config.present_mode = DisplayPresentMode::Immediate;
    config.render_mode = DisplayRenderMode::Partial;
    config.color_format = LV_COLOR_FORMAT_RGB565;
    config.make_default = false;
    config.viewport.rotation = rotation_;
    config.buffer.lines = kOverlayBufferLines;
    if (display_manager.create_display(config, &ui_) != ESP_OK) {
        ui_ = nullptr;
        return false;
    }
    buildUi();
    return true;
}

void VideoInputScreen::closeOverlay() {
    if (!ui_) return;
    display_manager.delete_display(ui_);
    ui_ = nullptr;
    top_bar_ = nullptr;
    bottom_bar_ = nullptr;
    settings_ = nullptr;
    title_label_ = nullptr;
    volume_label_ = nullptr;
    volume_slider_ = nullptr;
}

void VideoInputScreen::buildUi() {
    lv_obj_t *screen = lv_display_get_screen_active(ui_);
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    shown_title_.clear();

    /* Styleless, so a press changes nothing and never invalidates the video. */
    lv_obj_t *video = lv_container_create(screen);
    lv_obj_set_size(video, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(video, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_fn(video, LV_EVENT_CLICKED, [this](lv_event_t *) {
        requestMode(mode_ == UiMode::Hidden ? UiMode::Bars : UiMode::Hidden);
    });

    top_bar_ = create_bar(screen, lv_pct(100), kBarHeight, LV_ALIGN_TOP_MID);
    title_label_ =
        media_top_bar_build(top_bar_, title_.c_str(), [this] { this->back(); }, nullptr).title;
    bottom_bar_ = create_bar(screen, lv_pct(100), kBarHeight, LV_ALIGN_BOTTOM_MID);
    buildBottomBar(bottom_bar_);

    settings_ = is_portrait(rotation_)
        ? create_bar(screen, lv_pct(100), kPortraitPanelHeight, LV_ALIGN_BOTTOM_MID)
        : create_bar(screen, kLandscapePanelWidth, lv_pct(100), LV_ALIGN_RIGHT_MID);
    player_settings_panel_build(settings_, [this] { requestMode(UiMode::Bars); });

    lv_obj_set_flag(top_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_set_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_set_flag(settings_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Settings);

    /* A bar sits where it was created until the layout runs, and every area it
     * leaves on the way is painted with the screen behind it -- black, over the
     * video. Settle the layout here, where the invalidations can still be
     * dropped, so only the final areas are ever drawn. */
    lv_obj_update_layout(screen);
}

void VideoInputScreen::buildBottomBar(lv_obj_t *parent) {
    lv_obj_set_style_pad_hor(parent, kBarPadding, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *mute = media_icon_button(parent, kIconButton, &icon_36, TABLER_VOLUME,
                                       lv_color_white(), &volume_label_);
    volume_slider_ = media_slider(parent, 100, lv_color_white(), lv_color_hex(kTrackColor));
    lv_obj_set_width(volume_slider_, kVolumeSlider);
    media_volume_bind(mute, volume_label_, volume_slider_);

    lv_obj_t *settings = media_icon_button(parent, kIconButton, &icon_36,
                                           TABLER_ADJUSTMENTS_HORIZONTAL, lv_color_white());
    lv_obj_add_event_fn(settings, LV_EVENT_CLICKED, [this](lv_event_t *) {
        requestMode(UiMode::Settings);
    });
}

void VideoInputScreen::setMode(UiMode mode) {
    if (!ui_ || mode == mode_) return;
    mode_ = mode;

    /* The UI leaving an area has to be painted out before the video takes it
     * back, and the video has to be clipped out of an area before the UI is
     * drawn into it. */
    if (mode != UiMode::Bars) {
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (mode != UiMode::Settings) lv_obj_add_flag(settings_, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(ui_);
    bsp_display_wait_draw();
    video_presenter_set_ui_insets(insets());

    if (mode == UiMode::Bars) {
        media_volume_show(volume_label_, volume_slider_, audio_output_volume());
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    } else if (mode == UiMode::Settings) {
        lv_obj_send_event(settings_, LV_EVENT_REFRESH, nullptr);
        lv_obj_remove_flag(settings_, LV_OBJ_FLAG_HIDDEN);
    }
    if (mode == UiMode::Hidden) return;
    lv_display_trigger_activity(ui_);
    refresh();
}

void VideoInputScreen::requestMode(UiMode mode) {
    lv_async_call([this, mode] {
        if (s_active == this) setMode(mode);
    });
}

void VideoInputScreen::rotate(bsp_rotation_t rotation) {
    if (rotation == rotation_) return;
    rotation_ = rotation;
    video_presenter_set_rotation(rotation);
    if (!ui_) return;

    /* The rotation resizes the screen, which invalidates all of it. Only the
     * bars are redrawn from here; the video clears and repaints its own area. */
    lv_refr_now(ui_);
    lv_display_enable_invalidation(ui_, false);
    display_manager.set_rotation(ui_, rotation);
    buildUi();
    lv_display_enable_invalidation(ui_, true);
    video_presenter_set_ui_insets(insets());
    if (mode_ == UiMode::Bars) {
        lv_obj_invalidate(top_bar_);
        lv_obj_invalidate(bottom_bar_);
    } else if (mode_ == UiMode::Settings) {
        lv_obj_invalidate(settings_);
    }
    refresh();
}

void VideoInputScreen::unplugged() {
    if (s_active && !s_active->camera_->connected()) s_active->back();
}

void VideoInputScreen::onEnter() {
    s_active = this;
    media_cache_stop();
    rotation_ = ui_orientation_current();
    mode_ = UiMode::Hidden;
    error_.clear();
    if (!openOverlay()) {
        showStartError({});
        return;
    }

    const SharedSram sram = media_player_acquire_sram();
    if (!video_presenter_begin(sram, rotation_)) {
        closeOverlay();
        media_player_release_sram();
        showStartError(video_presenter_error());
        return;
    }
    video_presenter_set_ui_insets(insets());
    ui_orientation_set_listener([](bsp_rotation_t rotation, void *arg) {
        static_cast<VideoInputScreen *>(arg)->rotate(rotation);
    }, this);

    /* The first LVGL pass covers the whole screen, video area included. Let it
     * land before any frame can. */
    lv_refr_now(ui_);
    bsp_display_wait_draw();

    if (!startCapture(&error_)) setMode(UiMode::Bars);
    if (auto capture = media_player_capture_audio()) {
        std::string error;
        if (!audio_.start(std::move(capture), &error)) ESP_LOGW(TAG, "%s", error.c_str());
    }
    timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<VideoInputScreen *>(lv_timer_get_user_data(timer))->tick();
    }, kRefreshPeriodMs, this);
}

void VideoInputScreen::onExit() {
    if (s_active == this) s_active = nullptr;
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (ui_) {
        stopCapture();
        closeOverlay();
        video_presenter_end();
        ui_orientation_set_listener(nullptr, nullptr);
        media_player_release_sram();
    }
    media_cache_start();
}

VideoInputScreen::~VideoInputScreen() {
    if (timer_) lv_timer_delete(timer_);
    stopCapture();
    closeOverlay();
}

void VideoInputScreen::showStartError(const std::string &message) {
    auto modal = lv_modal_open(root_);
    lv_modal_title_create(modal, kTitle);
    lv_modal_message_create(modal, message.empty() ? "video output unavailable" : message.c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY, [this, modal](lv_event_t *) {
        lv_modal_close(modal);
        back();
    });
}

bool VideoInputScreen::startCapture(std::string *error) {
    TrackInfo track;
    track.codec = CodecId::Mjpeg;
    track.width = kCaptureWidth;
    track.height = kCaptureHeight;
    if (!video_presenter_open_stream(track, error)) return false;

    player_close();
    const media_arena_t arena = player_arena();
    if (!arena.data || arena.size < kSlots * kSlotBytes) {
        *error = "no memory for camera frames";
        return false;
    }
    uint8_t *slots[kSlots];
    for (std::size_t i = 0; i < kSlots; i++) slots[i] = arena.data + i * kSlotBytes;
    const esp_err_t err = camera_->start(kCaptureWidth, kCaptureHeight, kCaptureInterval, slots,
                                         kSlots, kSlotBytes);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        *error = "camera has no 1280x720 MJPEG at 30 fps";
        return false;
    }
    if (err != ESP_OK) {
        *error = std::string("camera did not start: ") + esp_err_to_name(err);
        return false;
    }

    feed_stopped_ = xSemaphoreCreateBinary();
    feed_quit_ = false;
#ifdef ESP_PLATFORM
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        feedMain, "video_input", kFeedStackBytes, this, kFeedPriority, nullptr, tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM);
#else
    const BaseType_t created =
        xTaskCreate(feedMain, "video_input", kFeedStackBytes, this, kFeedPriority, nullptr);
#endif
    if (feed_stopped_ && created == pdPASS) {
#ifdef ESP_PLATFORM
        if (!stats_timer_) {
            esp_timer_create_args_t args = {};
            args.callback = logStats;
            args.arg = this;
            args.name = "video_input_stats";
            esp_timer_create(&args, &stats_timer_);
        }
        if (stats_timer_) esp_timer_start_periodic(stats_timer_, 1000000);
#endif
        return true;
    }
    if (feed_stopped_) vSemaphoreDelete(feed_stopped_);
    feed_stopped_ = nullptr;
    camera_->stop();
    *error = "no memory for the capture task";
    return false;
}

void VideoInputScreen::stopCapture() {
#ifdef ESP_PLATFORM
    if (stats_timer_) {
        esp_timer_stop(stats_timer_);
        esp_timer_delete(stats_timer_);
        stats_timer_ = nullptr;
    }
#endif
    audio_.stop();
    feed_quit_ = true;
    camera_->stop();
    if (!feed_stopped_) return;
    xSemaphoreTake(feed_stopped_, portMAX_DELAY);
    vSemaphoreDelete(feed_stopped_);
    feed_stopped_ = nullptr;
}

void VideoInputScreen::feedMain(void *arg) {
    auto *self = static_cast<VideoInputScreen *>(arg);
    while (!self->feed_quit_) {
        usb_host::UvcFrame frame;
        self->feed_stage_ = 1;
        const esp_err_t err = self->camera_->receive(&frame, kReceiveTimeoutMs);
        self->feed_stage_ = 0;
        if (err == ESP_ERR_TIMEOUT) {
            self->receive_timeouts_++;
            continue;
        }
        if (err != ESP_OK) {
            self->last_receive_err_ = err;
            ESP_LOGW(TAG, "receive: %s, feeding stops", esp_err_to_name(err));
            break;
        }
        self->received_++;
        Held &held = self->held_[frame.slot];
        held.screen = self;
        held.camera = self->camera_.get();
        held.frame = frame;
        self->feed_stage_ = 2;
        const bool submitted =
            video_presenter_submit(frame.data, frame.size, releaseFrame, &held, true, 0);
        self->feed_stage_ = 0;
        if (submitted) {
            self->submitted_++;
        } else {
            self->submit_failed_++;
            self->camera_->release(frame);
        }
    }
    self->feed_stage_ = 3;
    xSemaphoreGive(self->feed_stopped_);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

void VideoInputScreen::releaseFrame(void *ctx) {
    auto *held = static_cast<Held *>(ctx);
    held->screen->released_++;
    held->camera->release(held->frame);
}

void VideoInputScreen::logStats(void *arg) {
    auto *self = static_cast<VideoInputScreen *>(arg);
    static const char *const kStages[] = { "idle", "receive", "submit", "stopped" };
    ESP_LOGI(TAG, "feed: %s, %u received, %u timeouts, %u submitted, %u submit failed, "
                  "%u released, last err %s",
             kStages[self->feed_stage_.load()], (unsigned)self->received_.exchange(0),
             (unsigned)self->receive_timeouts_.exchange(0),
             (unsigned)self->submitted_.exchange(0), (unsigned)self->submit_failed_.exchange(0),
             (unsigned)self->released_.exchange(0), esp_err_to_name(self->last_receive_err_.load()));
}

void VideoInputScreen::tick() {
    refresh();
    if (mode_ != UiMode::Bars) return;
    if (volume_slider_ && lv_obj_has_state(volume_slider_, LV_STATE_PRESSED)) return;
    if (lv_display_get_inactive_time(nullptr) < kAutoHideMs) return;
    setMode(UiMode::Hidden);
}

void VideoInputScreen::refresh() {
    if (!title_label_ || mode_ != UiMode::Bars) return;
    std::string title = error_;
    if (title.empty()) title = video_presenter_error();
    const bool message = !title.empty();
    if (!message) title = title_;
    if (title == shown_title_) return;
    shown_title_ = title;
    lv_label_set_text(title_label_, title.c_str());
    lv_obj_set_style_text_color(title_label_, message ? lv_color_hex(kMessageColor)
                                                      : lv_color_white(), 0);
}
