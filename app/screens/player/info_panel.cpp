/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "info_panel.hpp"
#include "panel.hpp"
#include "screens/home/settings_widgets.hpp"
#include "widgets.hpp"

#include <cmath>
#include <cstdio>

static constexpr int32_t kValueGap = 16;
static constexpr int32_t kValueLines = 3;

static void add_row(lv_obj_t *section, const char *label, const char *text) {
    auto row = lv_setting_row_create(section, label);
    auto value = lv_setting_value_create(row, &kPanelColors);
    lv_label_set_text(value, text);
}

static void add_wide_row(lv_obj_t *section, const char *label, const char *text, int32_t width) {
    const lv_font_t *font = lv_widgets_body_font();

    auto row = lv_container_create(section, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, kValueGap, 0);

    auto title = lv_label_create(row);
    lv_obj_set_style_text_font(title, font, 0);
    lv_label_set_text(title, label);

    auto value = lv_setting_value_create(row, &kPanelColors);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(value, text);

    const int32_t letter_space = lv_obj_get_style_text_letter_space(value, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(value, LV_PART_MAIN);
    const int32_t line_height = lv_font_get_line_height(font);
    lv_point_t title_size;
    lv_text_get_size(&title_size, label, font, letter_space, line_space, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_point_t size;
    lv_text_get_size(&size, text, font, letter_space, line_space,
                     width - title_size.x - kValueGap, LV_TEXT_FLAG_NONE);

    const int32_t limit = kValueLines * line_height + (kValueLines - 1) * line_space;
    if (size.y > limit) {
        lv_obj_set_height(value, limit);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    }
}

static void format_size(char *out, size_t size, int64_t bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(out, size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, size, "%.1f KB", (double)bytes / 1024.0);
    }
}

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

static void build_general(lv_obj_t *contents, const std::string &name,
                          const MediaSummary &summary) {
    auto section = lv_setting_section_create(contents, "General", &kPanelColors);
    char text[64];

    lv_obj_update_layout(section);
    add_wide_row(section, "File", name.c_str(), lv_obj_get_content_width(section));
    if (!summary.valid) return;

    add_row(section, "Format", summary.container);
    format_size(text, sizeof(text), summary.file_bytes);
    add_row(section, "Size", text);
    format_duration(text, sizeof(text), summary.duration_us);
    add_row(section, "Duration", text);
    if (summary.file_bytes > 0 && summary.duration_us > 0) {
        format_bitrate(text, sizeof(text),
                       summary.file_bytes * 8 * 1000000 / summary.duration_us);
        add_row(section, "Bitrate", text);
    }
    add_row(section, "Seek", summary.seekable ? "Supported" : "Unsupported");
}

static void build_video(lv_obj_t *contents, const MediaSummary &summary) {
    auto section = lv_setting_section_create(contents, "Video", &kPanelColors);
    char text[64];

    add_row(section, "Codec", codec_name(summary.video.codec));
    const char *profile = h264_profile_name(summary.video.profile_idc);
    if (profile) {
        snprintf(text, sizeof(text), "%s @ Level %d.%d", profile, summary.video.level_idc / 10,
                 summary.video.level_idc % 10);
        add_row(section, "Profile", text);
    }
    snprintf(text, sizeof(text), "%u x %u", (unsigned)summary.video.width,
             (unsigned)summary.video.height);
    add_row(section, "Resolution", text);
    if (summary.video.frame_interval_us > 0) {
        format_frame_rate(text, sizeof(text), summary.video.frame_interval_us);
        add_row(section, "Frame Rate", text);
    }
    const int degrees = rotation_degrees(summary.video.rotation);
    if (degrees) {
        snprintf(text, sizeof(text), "%d degrees", degrees);
        add_row(section, "Rotation", text);
    }
}

static void build_audio(lv_obj_t *contents, const MediaSummary &summary) {
    auto section = lv_setting_section_create(contents, "Audio", &kPanelColors);
    char text[64];

    add_row(section, "Codec", codec_name(summary.audio.codec));
    if (summary.audio.codec == CodecId::None) return;

    if (summary.audio.sample_rate) {
        snprintf(text, sizeof(text), "%.1f kHz", (double)summary.audio.sample_rate / 1000.0);
        add_row(section, "Sample Rate", text);
    }
    if (summary.audio.channels) {
        const char *layout = summary.audio.channels == 1 ? " (Mono)"
                           : summary.audio.channels == 2 ? " (Stereo)"
                                                         : "";
        snprintf(text, sizeof(text), "%u%s", (unsigned)summary.audio.channels, layout);
        add_row(section, "Channels", text);
    }
    if (summary.audio.bits &&
        (summary.audio.codec == CodecId::Pcm || summary.audio.codec == CodecId::AdpcmIma)) {
        snprintf(text, sizeof(text), "%u bit", (unsigned)summary.audio.bits);
        add_row(section, "Bit Depth", text);
    }
    if (summary.audio.bitrate_bps) {
        format_bitrate(text, sizeof(text), summary.audio.bitrate_bps);
    } else {
        snprintf(text, sizeof(text), "--");
    }
    add_row(section, "Bitrate", text);
    if (!summary.audio.note.empty()) {
        lv_obj_update_layout(section);
        add_wide_row(section, "Note", summary.audio.note.c_str(),
                     lv_obj_get_content_width(section));
    }
}

void player_info_panel_build(lv_obj_t *root, const std::string &name, const MediaSummary &summary,
                             std::function<void()> on_close,
                             std::function<void(bool)> on_scroll) {
    auto contents = player_panel_build(root, "Media Info", std::move(on_close));
    lv_obj_set_scroll_dir(contents, LV_DIR_VER);
    lv_obj_add_event_fn(contents, LV_EVENT_SCROLL_BEGIN,
                        [on_scroll](lv_event_t *) { on_scroll(true); });
    lv_obj_add_event_fn(contents, LV_EVENT_SCROLL_END,
                        [on_scroll](lv_event_t *) { on_scroll(false); });

    build_general(contents, name, summary);
    if (!summary.valid) return;
    build_video(contents, summary);
    build_audio(contents, summary);
}
