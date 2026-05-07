#pragma once
#include <tuple>
#include <optional>

namespace pf_port {

enum class PixelFormat {
    RGB565,
    RGB888,
};

void init(int fb_num, PixelFormat pixel_format);
void display_set_brightness(int value);
void *display_get_frame_buffer(int fb_index);
void display_flush(int fb_index);
std::optional<std::tuple<int, int>> touch_get_point();

}
