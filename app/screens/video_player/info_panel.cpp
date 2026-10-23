/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "info_panel.hpp"
#include "screens/player_panel.hpp"
#include "screens/home/settings_widgets.hpp"
#include "widgets.hpp"
#include "esp_heap_caps.h"

#include <cmath>
#include <cstdio>

static constexpr uint32_t kCacheAlignment = 64;

struct PanelCache {
    lv_image_dsc_t dsc;
    void *pixels;
};

static void format_duration(char *out, size_t size, int64_t duration_us) {
    const int64_t seconds = duration_us / 1000000;
    snprintf(out, size, "%d:%02d:%02d", (int)(seconds / 3600), (int)(seconds / 60 % 60),
             (int)(seconds % 60));
}

static void format_bitrate(char *out, size_t size, int64_t bps) {
    if (bps >= 1000000) {
        snprintf(out, size, "%.2f Mbps", (double)bps / 1000000.0);
    } else {
        snprintf(out, size, "%d kbps", (int)((bps + 500) / 1000));
    }
}

static void format_frame_rate(char *out, size_t size, int64_t frame_interval_us) {
    const double fps = 1000000.0 / (double)frame_interval_us;
    if (std::fabs(fps - std::round(fps)) < 0.01) {
        snprintf(out, size, "%d fps", (int)std::lround(fps));
    } else {
        snprintf(out, size, "%.2f fps", fps);
    }
}

static int rotation_degrees(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90: return 90;
    case BSP_ROTATION_180: return 180;
    case BSP_ROTATION_270: return 270;
    default: return 0;
    }
}

static const char *h264_profile_name(uint8_t profile_idc) {
    switch (profile_idc) {
    case 66: return "Baseline";
    case 77: return "Main";
    case 88: return "Extended";
    case 100: return "High";
    case 110: return "High 10";
    case 122: return "High 4:2:2";
    case 244: return "High 4:4:4";
    default: return nullptr;
    }
}

static PanelSection general_section(const std::string &name, const MediaSummary &summary) {
    PanelSection section = { "General", {} };
    auto &rows = section.rows;
    char text[64];

    rows.push_back({ "File", name, true });
    if (!summary.valid) return section;

    rows.push_back({ "Format", summary.container });
    panel_format_size(text, sizeof(text), summary.file_bytes);
    rows.push_back({ "Size", text });
    format_duration(text, sizeof(text), summary.duration_us);
    rows.push_back({ "Duration", text });
    if (summary.file_bytes > 0 && summary.duration_us > 0) {
        format_bitrate(text, sizeof(text),
                       summary.file_bytes * 8 * 1000000 / summary.duration_us);
        rows.push_back({ "Bitrate", text });
    }
    rows.push_back({ "Seek", summary.seekable ? "Supported" : "Unsupported" });
    return section;
}

static PanelSection tags_section(const MediaTags &tags) {
    PanelSection section = { "Tags", {} };
    auto &rows = section.rows;
    if (!tags.title.empty()) rows.push_back({ "Title", tags.title, true });
    if (!tags.artist.empty()) rows.push_back({ "Artist", tags.artist, true });
    if (!tags.album.empty()) rows.push_back({ "Album", tags.album, true });
    if (!tags.album_artist.empty()) rows.push_back({ "Album Artist", tags.album_artist, true });
    if (!tags.track.empty()) rows.push_back({ "Track", tags.track });
    if (!tags.date.empty()) rows.push_back({ "Date", tags.date });
    return section;
}

static PanelSection video_section(const MediaSummary &summary) {
    PanelSection section = { "Video", {} };
    auto &rows = section.rows;
    char text[64];

    rows.push_back({ "Codec", codec_name(summary.video.codec) });
    const char *profile = h264_profile_name(summary.video.profile_idc);
    if (profile) {
        snprintf(text, sizeof(text), "%s @ Level %d.%d", profile, summary.video.level_idc / 10,
                 summary.video.level_idc % 10);
        rows.push_back({ "Profile", text });
    }
    snprintf(text, sizeof(text), "%u x %u", (unsigned)summary.video.width,
             (unsigned)summary.video.height);
    rows.push_back({ "Resolution", text });
    if (summary.video.frame_interval_us > 0) {
        format_frame_rate(text, sizeof(text), summary.video.frame_interval_us);
        rows.push_back({ "Frame Rate", text });
    }
    const int degrees = rotation_degrees(summary.video.rotation);
    if (degrees) {
        snprintf(text, sizeof(text), "%d degrees", degrees);
        rows.push_back({ "Rotation", text });
    }
    return section;
}

static PanelSection audio_section(const MediaSummary &summary) {
    PanelSection section = { "Audio", {} };
    auto &rows = section.rows;
    char text[64];

    rows.push_back({ "Codec", codec_name(summary.audio.codec) });
    if (summary.audio.codec == CodecId::None) return section;

    if (summary.audio.sample_rate) {
        snprintf(text, sizeof(text), "%.1f kHz", (double)summary.audio.sample_rate / 1000.0);
        rows.push_back({ "Sample Rate", text });
    }
    if (summary.audio.channels) {
        const char *layout = summary.audio.channels == 1 ? " (Mono)"
                           : summary.audio.channels == 2 ? " (Stereo)"
                                                         : "";
        snprintf(text, sizeof(text), "%u%s", (unsigned)summary.audio.channels, layout);
        rows.push_back({ "Channels", text });
    }
    if (summary.audio.bits &&
        (summary.audio.codec == CodecId::Pcm || summary.audio.codec == CodecId::AdpcmIma)) {
        snprintf(text, sizeof(text), "%u bit", (unsigned)summary.audio.bits);
        rows.push_back({ "Bit Depth", text });
    }
    if (summary.audio.bitrate_bps) {
        format_bitrate(text, sizeof(text), summary.audio.bitrate_bps);
    } else {
        snprintf(text, sizeof(text), "--");
    }
    rows.push_back({ "Bitrate", text });
    if (!summary.audio.note.empty()) rows.push_back({ "Note", summary.audio.note, true });
    return section;
}

static std::vector<PanelSection> info_sections(const std::string &name,
                                               const MediaSummary &summary, bool video) {
    std::vector<PanelSection> sections = { general_section(name, summary) };
    if (!summary.valid) return sections;
    if (!summary.tags.empty()) sections.push_back(tags_section(summary.tags));
    if (video) sections.push_back(video_section(summary));
    sections.push_back(audio_section(summary));
    return sections;
}

static void cache_contents(lv_obj_t *contents) {
    lv_obj_update_layout(contents);
    const int32_t width = lv_obj_get_width(contents);
    const int32_t shown = lv_obj_get_height(contents);
    const int32_t height = shown + lv_obj_get_scroll_bottom(contents);
    if (width <= 0 || height <= shown) return;

    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    const size_t bytes = (size_t)stride * height;
    void *pixels = heap_caps_aligned_alloc(kCacheAlignment, bytes, MALLOC_CAP_SPIRAM);
    if (!pixels) return;

    lv_display_t *previous = lv_display_get_default();
    lv_display_t *offscreen = lv_display_create(width, height);
    if (!offscreen) {
        heap_caps_free(pixels);
        return;
    }
    lv_display_set_color_format(offscreen, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(offscreen, pixels, nullptr, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(offscreen, [](lv_display_t *display, const lv_area_t *, uint8_t *) {
        lv_display_flush_ready(display);
    });

    lv_obj_t *page = lv_display_get_screen_active(offscreen);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_text_color(page, lv_color_white(), 0);
    lv_setting_page_style(page, &kPanelColors);
    while (lv_obj_get_child_count(contents)) {
        lv_obj_set_parent(lv_obj_get_child(contents, 0), page);
    }
    lv_obj_invalidate(page);
    lv_refr_now(offscreen);
    lv_display_delete(offscreen);
    lv_display_set_default(previous);

    auto cache = new PanelCache{};
    cache->pixels = pixels;
    cache->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cache->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    cache->dsc.header.w = width;
    cache->dsc.header.h = height;
    cache->dsc.header.stride = stride;
    cache->dsc.data = static_cast<const uint8_t *>(pixels);
    cache->dsc.data_size = bytes;

    lv_obj_set_style_pad_all(contents, 0, 0);
    lv_obj_t *image = lv_image_create(contents);
    lv_image_set_src(image, &cache->dsc);
    lv_obj_add_event_cb(image, [](lv_event_t *event) {
        auto stale = static_cast<PanelCache *>(lv_event_get_user_data(event));
        heap_caps_free(stale->pixels);
        delete stale;
    }, LV_EVENT_DELETE, cache);
}

void player_info_panel_build(lv_obj_t *root, const std::string &name, const MediaSummary &summary,
                             std::function<void()> on_close) {
    auto contents = player_panel_build(root, "Media Info", std::move(on_close));
    lv_obj_set_scroll_dir(contents, LV_DIR_VER);

    panel_sections_show(contents, info_sections(name, summary, true));
    if (summary.valid) cache_contents(contents);
}

void player_info_sections_show(lv_obj_t *contents, const std::string &name,
                               const MediaSummary &summary, const SettingColors *colors) {
    panel_sections_show(contents,
                        info_sections(name, summary, summary.video.codec != CodecId::None),
                        colors);
}
