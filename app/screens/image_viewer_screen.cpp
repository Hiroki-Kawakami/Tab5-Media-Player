/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "image_viewer_screen.hpp"
#include "media/image_codec.hpp"
#include "media_player.hpp"
#include "screens/image_object.hpp"
#include "screens/image_viewer/bgm_picker_screen.hpp"
#include "screens/image_viewer/info_panel.hpp"
#include "screens/image_viewer/slideshow_panel.hpp"
#include "screens/media_controls.hpp"
#include "screens/video_player/settings_panel.hpp"
#include "settings.hpp"
#include "slideshow/slideshow.hpp"
#include "screen_manager.hpp"
#include "bsp.h"
#include "driver/ppa.h"
#include "esp_log.h"
#include "resources.h"

#include <algorithm>
#include <cstdlib>

static const char *TAG = "image_viewer";

static constexpr int32_t kBarHeight = 80;
static constexpr int32_t kBarPadding = 8;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kSwipeThreshold = 80;
static constexpr int32_t kPortraitPanelHeight = 640;
static constexpr int32_t kLandscapePanelWidth = 560;

static constexpr uint32_t kScaleDenominator = 16;

static constexpr uint32_t kBarColor = 0x101010;
static constexpr uint32_t kMessageColor = 0xffb74d;

static ImageViewerScreen *s_active;
static ppa_client_handle_t s_srm;

static bool panel_rgb888() {
    return bsp_display_get_pixel_format() == BSP_PIXEL_FORMAT_RGB888;
}

static uint8_t *framebuffer(int index) {
    return static_cast<uint8_t *>(bsp_display_get_frame_buffer(index));
}

static std::size_t framebuffer_bytes() {
    const bsp_size_t size = bsp_display_get_size();
    return (std::size_t)size.width * size.height * (panel_rgb888() ? 3 : 2);
}

/* LVGL draws through framebuffer 0 only, so 1 and 2 hold the picture. */
static int spare_framebuffer(int shown) {
    return shown == 1 ? 2 : 1;
}

static SlideshowPanelValues slideshow_values() {
    return { (uint32_t)settings_slideshow_interval(), settings_slideshow_shuffle(),
             settings_slideshow_transition(),
             settings_slideshow_direction(), settings_slideshow_hold_to_exit(),
             settings_slideshow_bgm(),
             settings_slideshow_bgm_shuffle(), settings_slideshow_bgm_path() };
}

static lv_obj_t *create_bar(lv_obj_t *parent, int32_t width, int32_t height, lv_align_t align) {
    lv_obj_t *bar = lv_container_create(parent, lv_color_hex(kBarColor));
    lv_obj_set_size(bar, width, height);
    lv_obj_align(bar, align, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

ImageViewerScreen::~ImageViewerScreen() {
    if (s_active == this) s_active = nullptr;
}

void ImageViewerScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_fn(root_, LV_EVENT_SIZE_CHANGED, [this](lv_event_t *) {
        if ((lv_obj_get_width(root_) > lv_obj_get_height(root_)) == landscape_) return;
        lv_async_call([this] {
            if (s_active != this) return;
            media_cache_cancel(token_);
            media_cache_cancel(idle_token_);
            buildUi();
            load();
        });
    });
    buildUi();
}

void ImageViewerScreen::buildUi() {
    lv_obj_clean(root_);
    image_ = nullptr;
    landscape_ = lv_obj_get_width(root_) > lv_obj_get_height(root_);

    stage_ = lv_container_create(root_);
    lv_obj_set_size(stage_, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(stage_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_fn(stage_, LV_EVENT_PRESSED, [this](lv_event_t *event) { handlePress(event); });
    lv_obj_add_event_fn(stage_, LV_EVENT_PRESSING, [this](lv_event_t *event) { handleMove(event); });
    lv_obj_add_event_fn(stage_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        if (swiped_) return;
        requestMode(mode_ == UiMode::Hidden ? UiMode::Bars : UiMode::Hidden);
    });

    message_ = lv_label_create(stage_);
    lv_obj_set_width(message_, lv_pct(90));
    lv_label_set_long_mode(message_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_font_role(message_, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(message_);
    lv_label_set_text(message_, "");

    top_bar_ = create_bar(root_, lv_pct(100), kBarHeight, LV_ALIGN_TOP_MID);
    const MediaTopBar bar = media_top_bar_build(top_bar_, name().c_str(),
                                                [this] { this->back(); },
                                                [this] { requestMode(UiMode::Info); });
    title_label_ = bar.title;
    info_button_ = bar.info_button;

    bottom_bar_ = create_bar(root_, lv_pct(100), kBarHeight, LV_ALIGN_BOTTOM_MID);
    buildBottomBar(bottom_bar_);
    buildPanels();

    lv_obj_set_flag(top_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_set_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_set_flag(slideshow_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Slideshow);
    lv_obj_set_flag(settings_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Settings);
    lv_obj_set_flag(info_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Info);
    lv_obj_update_layout(root_);

    if (shown_fb_ >= 0) createImage();
}

void ImageViewerScreen::buildBottomBar(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(parent, kBarPadding, 0);
    lv_obj_set_style_pad_column(parent, kBarPadding, 0);

    prev_button_ = media_icon_button(parent, kIconButton, &icon_48, TABLER_CIRCLE_ARROW_LEFT,
                                     lv_color_white());
    lv_obj_add_event_fn(prev_button_, LV_EVENT_CLICKED, [this](lv_event_t *) { requestAdvance(-1); });

    next_button_ = media_icon_button(parent, kIconButton, &icon_48, TABLER_CIRCLE_ARROW_RIGHT,
                                     lv_color_white());
    lv_obj_add_event_fn(next_button_, LV_EVENT_CLICKED, [this](lv_event_t *) { requestAdvance(1); });

    lv_spacer_create(parent, 1, 1, 1);

    slideshow_button_ = media_icon_button(parent, kIconButton, &icon_36, TABLER_SLIDESHOW,
                                          lv_color_white());
    lv_obj_add_event_fn(slideshow_button_, LV_EVENT_CLICKED,
                        [this](lv_event_t *) { requestMode(UiMode::Slideshow); });

    panel_button_ = media_icon_button(parent, kIconButton, &icon_36,
                                      TABLER_ADJUSTMENTS_HORIZONTAL, lv_color_white());
    lv_obj_add_event_fn(panel_button_, LV_EVENT_CLICKED,
                        [this](lv_event_t *) { requestMode(UiMode::Settings); });

    counter_label_ = lv_label_create(parent);
    lv_obj_add_flag(counter_label_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_font_role(counter_label_, LV_WIDGETS_FONT_BODY);
    lv_obj_align(counter_label_, LV_ALIGN_CENTER, 0, 0);

    updateTransport();
}

void ImageViewerScreen::buildPanels() {
    const bool portrait = !landscape_;
    slideshow_ = portrait
        ? create_bar(root_, lv_pct(100), kPortraitPanelHeight, LV_ALIGN_BOTTOM_MID)
        : create_bar(root_, kLandscapePanelWidth, lv_pct(100), LV_ALIGN_RIGHT_MID);
    image_slideshow_panel_build(slideshow_, slideshow_values(),
                                [](const SlideshowPanelValues &changed) {
        settings_set_slideshow_interval((int)changed.interval_s);
        settings_set_slideshow_shuffle(changed.shuffle);
        settings_set_slideshow_transition(changed.transition);
        settings_set_slideshow_direction(changed.direction);
        settings_set_slideshow_hold_to_exit(changed.hold_to_exit);
        settings_set_slideshow_bgm(changed.bgm);
        settings_set_slideshow_bgm_shuffle(changed.bgm_shuffle);
        settings_commit();
    }, [this] { chooseBgm(); }, [this] {
        lv_async_call([this] {
            if (s_active == this) startSlideshow();
        });
    }, [this] { requestMode(UiMode::Bars); });

    settings_ = portrait
        ? create_bar(root_, lv_pct(100), kPortraitPanelHeight, LV_ALIGN_BOTTOM_MID)
        : create_bar(root_, kLandscapePanelWidth, lv_pct(100), LV_ALIGN_RIGHT_MID);
    player_settings_panel_build(settings_, [this] { requestMode(UiMode::Bars); },
                                PlayerSettingsDisplay);

    info_ = portrait
        ? create_bar(root_, lv_pct(100), kPortraitPanelHeight, LV_ALIGN_BOTTOM_MID)
        : create_bar(root_, kLandscapePanelWidth, lv_pct(100), LV_ALIGN_RIGHT_MID);
    if (mode_ == UiMode::Info) populateInfo();
}

void ImageViewerScreen::chooseBgm() {
    const std::string &saved = settings_slideshow_bgm_path();
    screen_manager.push(std::make_shared<BgmPickerScreen>(
        saved.empty() ? path() : saved, [this](const std::string &picked) {
            settings_set_slideshow_bgm_path(picked);
            settings_commit();
            const SlideshowPanelValues values = slideshow_values();
            if (slideshow_) lv_obj_send_event(slideshow_, LV_EVENT_REFRESH, (void *)&values);
        }));
}

void ImageViewerScreen::refreshInfo() {
    if (mode_ == UiMode::Info) populateInfo();
}

void ImageViewerScreen::populateInfo() {
    if (!info_) return;
    lv_obj_clean(info_);
    image_info_panel_build(info_, name(), media_cache_lookup(path()).get(),
                           shown_fb_ >= 0 ? &shown_size_ : nullptr, panel_rgb888(),
                           [this] { requestMode(UiMode::Bars); });
    lv_obj_update_layout(info_);
}

void ImageViewerScreen::updateTransport() {
    if (counter_label_) {
        lv_label_set_text_fmt(counter_label_, "%d/%d", (int)playlist_->index() + 1,
                              (int)playlist_->size());
    }
    if (prev_button_) {
        lv_obj_set_state(prev_button_, LV_STATE_DISABLED,
                         !playlist_->canStep(-1, RepeatMode::Off));
    }
    if (next_button_) {
        lv_obj_set_state(next_button_, LV_STATE_DISABLED, !playlist_->canStep(1, RepeatMode::Off));
    }
    if (info_button_) {
        lv_obj_set_state(info_button_, LV_STATE_DISABLED, !media_cache_lookup(path()));
    }
}

void ImageViewerScreen::setMode(UiMode mode) {
    mode_ = mode;
    if (!top_bar_) return;
    if (mode == UiMode::Info) populateInfo();
    if (mode == UiMode::Settings) lv_obj_send_event(settings_, LV_EVENT_REFRESH, nullptr);
    lv_obj_set_flag(top_bar_, LV_OBJ_FLAG_HIDDEN, mode != UiMode::Bars);
    lv_obj_set_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN, mode != UiMode::Bars);
    lv_obj_set_flag(slideshow_, LV_OBJ_FLAG_HIDDEN, mode != UiMode::Slideshow);
    lv_obj_set_flag(settings_, LV_OBJ_FLAG_HIDDEN, mode != UiMode::Settings);
    lv_obj_set_flag(info_, LV_OBJ_FLAG_HIDDEN, mode != UiMode::Info);
}

void ImageViewerScreen::requestMode(UiMode mode) {
    lv_async_call([this, mode] {
        if (s_active == this) setMode(mode);
    });
}

void ImageViewerScreen::handlePress(lv_event_t *event) {
    swiped_ = false;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev) lv_indev_get_point(indev, &press_);
}

void ImageViewerScreen::handleMove(lv_event_t *event) {
    if (swiped_) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int32_t dx = point.x - press_.x;
    const int32_t dy = point.y - press_.y;
    if (abs(dx) < kSwipeThreshold || abs(dx) <= abs(dy)) return;
    swiped_ = true;
    requestAdvance(dx < 0 ? 1 : -1);
}

void ImageViewerScreen::requestAdvance(int delta) {
    lv_async_call([this, delta] {
        if (s_active != this || slideshow_running_) return;
        if (!playlist_->step(delta, RepeatMode::Off)) return;
        media_cache_cancel(token_);
        if (title_label_) lv_label_set_text(title_label_, name().c_str());
        load();
    });
}

void ImageViewerScreen::requestLoad() {
    lv_async_call([this] {
        if (s_active == this) load();
    });
}

void ImageViewerScreen::setMessage(const std::string &message, bool failed) {
    if (!message_) return;
    lv_label_set_text(message_, message.c_str());
    lv_obj_set_style_text_color(message_, failed ? lv_color_hex(kMessageColor) : lv_color_white(), 0);
    lv_obj_set_flag(message_, LV_OBJ_FLAG_HIDDEN, message.empty());
}

void ImageViewerScreen::createImage() {
    image_ = image_object_create(stage_, framebuffer(shown_fb_), shown_size_, panel_rgb888());
    if (!image_) return;
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(image_, 0);
}

void ImageViewerScreen::showFramebuffer(int index, ImageSize size) {
    if (image_) {
        lv_obj_delete(image_);
        image_ = nullptr;
    }
    shown_fb_ = index;
    shown_size_ = size;
    if (index < 0) {
        shown_path_.clear();
        return;
    }
    shown_path_ = path();
    createImage();
}

bool ImageViewerScreen::showCached() {
    const int spare = spare_framebuffer(shown_fb_);
    ImageSize size;
    if (!media_cache_read_image(path(), box_, framebuffer(spare), framebuffer_bytes(), &size)) {
        return false;
    }
    showFramebuffer(spare, size);
    return true;
}

bool ImageViewerScreen::showScaled() {
    if (!s_srm) return false;
    const ImageSize src = shown_size_;
    const uint32_t scale = std::min((uint32_t)box_.width * kScaleDenominator / src.width,
                                    (uint32_t)box_.height * kScaleDenominator / src.height);
    if (scale < 1 || scale > 16 * kScaleDenominator) return false;
    const ImageSize size = { (int16_t)(src.width * scale / kScaleDenominator),
                             (int16_t)(src.height * scale / kScaleDenominator) };
    if (!size.valid()) return false;

    const ppa_srm_color_mode_t mode =
        panel_rgb888() ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    const int spare = spare_framebuffer(shown_fb_);
    ppa_srm_oper_config_t op = {};
    op.in.buffer = framebuffer(shown_fb_);
    op.in.pic_w = src.width;
    op.in.pic_h = src.height;
    op.in.block_w = src.width;
    op.in.block_h = src.height;
    op.in.srm_cm = mode;
    op.out.buffer = framebuffer(spare);
    op.out.buffer_size = (uint32_t)framebuffer_bytes();
    op.out.pic_w = size.width;
    op.out.pic_h = size.height;
    op.out.srm_cm = mode;
    op.scale_x = (float)scale / kScaleDenominator;
    op.scale_y = op.scale_x;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa scale %dx%d -> %dx%d: %s", src.width, src.height, size.width,
                 size.height, esp_err_to_name(err));
        return false;
    }
    showFramebuffer(spare, size);
    return true;
}

void ImageViewerScreen::load() {
    if (slideshow_running_) return;
    updateTransport();
    const ImageSize previous = box_;
    box_ = { (int16_t)lv_obj_get_width(root_), (int16_t)lv_obj_get_height(root_) };
    if (showCached()) {
        setMessage({}, false);
        refreshInfo();
        prefetch();
        return;
    }

    /* A rotation leaves the same picture on screen at the wrong size. Rescaling
       what is already decoded is a PPA blit, so it stands in until the size the
       screen now wants has been decoded. */
    if (shown_fb_ >= 0 && shown_path_ == path() && !(previous == box_) && showScaled()) {
        setMessage({}, false);
    } else {
        showFramebuffer(-1, {});
        setMessage("Loading...\n" + name(), false);
    }
    refreshInfo();
    media_cache_request(path(), MetaWantInfo | MetaWantImage, box_, MetaPriority::Blocking, token_);
}

void ImageViewerScreen::prefetch() {
    media_cache_idle_cancel();
    for (int delta : { 1, -1 }) {
        if (!playlist_->canStep(delta, RepeatMode::Off)) continue;
        const std::size_t index = (std::size_t)((long long)playlist_->index() + delta);
        media_cache_request(playlist_->at(index).path, MetaWantInfo | MetaWantImage, box_,
                            MetaPriority::Idle, idle_token_);
    }
}

void ImageViewerScreen::imageReady(const std::string &path) {
    if (s_active && path == s_active->path()) s_active->showReady();
}

static std::string describe(const MediaEntry &entry) {
    std::string text;
    if (entry.image_width && entry.image_height) {
        text = std::to_string(entry.image_width) + "x" + std::to_string(entry.image_height);
    }
    const char *kind = image_kind_name(entry);
    if (!kind[0]) return text;
    if (!text.empty()) text += " ";
    return text + kind;
}

void ImageViewerScreen::showReady() {
    if (slideshow_running_) return;
    updateTransport();
    if (showCached()) {
        setMessage({}, false);
        refreshInfo();
        prefetch();
        return;
    }
    refreshInfo();
    /* Whatever else completed, the picture on screen is this file's: a
       completion that carries nothing cannot turn it into an error. */
    if (image_ && shown_path_ == path()) return;
    /* The picture may simply not be at this box yet: another request for the
       same file, at the size before a rotation, completes the same way. */
    auto entry = media_cache_lookup(path());
    if (!entry || (entry->ok && !entry->image_failed)) return;
    const char *reason = !entry->ok ? (entry->file_bytes ? "unsupported image format"
                                                         : "cannot read the file")
                       : entry->image_too_large ? "too large to decode"
                                                : "cannot decode the image";
    const std::string what = describe(*entry);
    setMessage(name() + "\n" + (what.empty() ? "" : what + "\n") + reason, true);
}

void ImageViewerScreen::startSlideshow() {
    if (slideshow_running_) return;
    media_cache_cancel(token_);
    media_cache_cancel(idle_token_);

    std::vector<PlaylistItem> pictures;
    pictures.reserve(playlist_->size());
    for (std::size_t i = 0; i < playlist_->size(); i++) pictures.push_back(playlist_->at(i));

    SlideshowConfig config;
    config.interval_ms = (uint32_t)settings_slideshow_interval() * 1000;
    config.transition = settings_slideshow_transition();
    config.direction = settings_slideshow_direction();
    config.shuffle = settings_slideshow_shuffle();
    config.hold_to_exit = settings_slideshow_hold_to_exit();
    config.bgm_shuffle = settings_slideshow_bgm_shuffle();
    std::vector<PlaylistItem> bgm;
    if (settings_slideshow_bgm()) {
        bgm = playlist_items_at(settings_slideshow_bgm_path(), MediaKind::Audio);
    }
    slideshow_running_ = true;
    const bool started = slideshow_start(
        std::move(pictures), playlist_->index(), box_, config, std::move(bgm),
        [this](std::size_t index) {
            if (s_active == this && slideshow_running_) endSlideshow(index);
        });
    if (started) return;
    slideshow_running_ = false;
    load();
}

void ImageViewerScreen::endSlideshow(std::size_t index) {
    slideshow_running_ = false;
    /* The slideshow has drawn over both picture framebuffers. */
    showFramebuffer(-1, {});
    playlist_->select(index);
    showCached();
    lv_obj_update_layout(root_);
    if ((lv_obj_get_width(root_) > lv_obj_get_height(root_)) != landscape_) buildUi();
    if (title_label_) lv_label_set_text(title_label_, name().c_str());
    load();
    setMode(UiMode::Bars);
}

void ImageViewerScreen::eject(const std::string &mount_point) {
    if (!s_active || !path_is_under(s_active->path(), mount_point)) return;
    if (screen_manager.current_screen() != s_active) screen_manager.pop();
    s_active->back();
}

void ImageViewerScreen::onEnter() {
    s_active = this;
    if (!token_) {
        token_ = media_cache_token();
        idle_token_ = media_cache_token();
    }
    if (!s_srm) {
        ppa_client_config_t client = {};
        client.oper_type = PPA_OPERATION_SRM;
        client.max_pending_trans_num = 1;
        const esp_err_t err = ppa_register_client(&client, &s_srm);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no ppa client: %s", esp_err_to_name(err));
            s_srm = nullptr;
        }
    }
    media_cache_observe(token_, imageReady);
    media_cache_idle_cancel();
    requestLoad();
}

void ImageViewerScreen::onExit() {
    if (slideshow_running_) {
        slideshow_running_ = false;
        slideshow_stop();
    }
    if (s_active == this) s_active = nullptr;
    media_cache_unobserve(token_);
    media_cache_cancel(token_);
    media_cache_cancel(idle_token_);
    showFramebuffer(-1, {});
    if (s_srm) {
        ppa_unregister_client(s_srm);
        s_srm = nullptr;
    }
}
