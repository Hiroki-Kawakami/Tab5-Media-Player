/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_codec.hpp"
#include "media/media_probe.hpp"

bool video_frame_decodable(CodecId codec);
ImageOrientation video_frame_orientation(bsp_rotation_t rotation);

/* H.264 and MPEG-2 only. Blocks for the whole decode, and holds a work arena
   and a frame in PSRAM while it runs. */
std::shared_ptr<ImagePixels> video_frame_decode(const VideoKeyframe &frame, ImageSize box,
                                                bool rgb888, ImageOrientation orientation);
