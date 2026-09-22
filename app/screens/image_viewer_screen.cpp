/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "image_viewer_screen.hpp"
#include "media/image_codec.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "screens/image_object.hpp"
#include "screens/media_controls.hpp"
#include "bsp.h"
#include "resources.h"

#include <cstdlib>

static constexpr int32_t kBarHeight = 80;
static constexpr int32_t kBarPadding = 8;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kSwipeThreshold = 80;

static constexpr uint32_t kBarColor = 0x101010;
static constexpr uint32_t kMessageColor = 0xffb74d;

static ImageViewerScreen *s_active;

static lv_obj_t *create_bar(lv_obj_t *parent, lv_align_t align) {
    lv_obj_t *bar = lv_container_create(parent, lv_color_hex(kBarColor));
    lv_obj_set_size(bar, lv_pct(100), kBarHeight);
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
    lv_obj_set_style_text_font(message_, lv_widgets_body_font(), 0);
    lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(message_);
    lv_label_set_text(message_, "");

    top_bar_ = create_bar(root_, LV_ALIGN_TOP_MID);
    const MediaTopBar bar = media_top_bar_build(top_bar_, name().c_str(),
                                                [this] { this->back(); }, [] {});
    title_label_ = bar.title;
    info_button_ = bar.info_button;

    bottom_bar_ = create_bar(root_, LV_ALIGN_BOTTOM_MID);
    buildBottomBar(bottom_bar_);

    lv_obj_set_state(info_button_, LV_STATE_DISABLED, true);
    lv_obj_set_state(panel_button_, LV_STATE_DISABLED, true);

    lv_obj_set_flag(top_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_set_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_update_layout(root_);

    if (pixels_) showPixels(pixels_);
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

    panel_button_ = media_icon_button(parent, kIconButton, &icon_36,
                                      TABLER_ADJUSTMENTS_HORIZONTAL, lv_color_white());

    counter_label_ = lv_label_create(parent);
    lv_obj_add_flag(counter_label_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(counter_label_, lv_widgets_body_font(), 0);
    lv_obj_align(counter_label_, LV_ALIGN_CENTER, 0, 0);

    updateTransport();
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
}

void ImageViewerScreen::setMode(UiMode mode) {
    mode_ = mode;
    if (!top_bar_) return;
    lv_obj_set_flag(top_bar_, LV_OBJ_FLAG_HIDDEN, mode != UiMode::Bars);
    lv_obj_set_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN, mode != UiMode::Bars);
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
        if (s_active != this) return;
        if (!playlist_->step(delta, RepeatMode::Off)) return;
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

void ImageViewerScreen::showPixels(std::shared_ptr<const ImagePixels> pixels) {
    if (image_) {
        lv_obj_delete(image_);
        image_ = nullptr;
    }
    pixels_ = std::move(pixels);
    if (!pixels_) return;
    image_ = image_object_create(stage_, pixels_);
    if (image_) {
        lv_obj_remove_flag(image_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(image_, 0);
    }
}

void ImageViewerScreen::load() {
    updateTransport();
    showPixels(nullptr);
    setMessage("Loading\n" + name(), false);
    lv_refr_now(nullptr);

    const bool rgb888 = bsp_display_get_pixel_format() == BSP_PIXEL_FORMAT_RGB888;
    ImageLoad result = image_decode_file(path(), lv_obj_get_width(root_),
                                         lv_obj_get_height(root_), rgb888, nullptr);
    if (!result.pixels) {
        setMessage(name() + "\n" + result.error, true);
        return;
    }
    setMessage({}, false);
    showPixels(std::move(result.pixels));
}

void ImageViewerScreen::eject(const std::string &mount_point) {
    if (s_active && path_is_under(s_active->path(), mount_point)) s_active->back();
}

void ImageViewerScreen::onEnter() {
    s_active = this;
    media_cache_stop();
    requestLoad();
}

void ImageViewerScreen::onExit() {
    if (s_active == this) s_active = nullptr;
    showPixels(nullptr);
    image_codec_close();
    media_cache_start();
}
