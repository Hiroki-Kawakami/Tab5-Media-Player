/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>

enum class CodecId {
    None,
    Mjpeg,
    Pcm,
    Mp3,
    Unsupported,
};

enum class TrackType {
    Video,
    Audio,
};

struct TrackInfo {
    CodecId codec = CodecId::None;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    uint32_t max_packet_bytes = 0;
};

struct MediaInfo {
    TrackInfo video;
    TrackInfo audio;
    int64_t duration_us = 0;
    int64_t frame_interval_us = 0;
    bool seekable = false;
};

struct Packet {
    TrackType track = TrackType::Video;
    int64_t pts_us = 0;
    bool keyframe = false;
    std::size_t len = 0;
};

inline const char *codec_name(CodecId codec) {
    switch (codec) {
    case CodecId::Mjpeg: return "MJPEG";
    case CodecId::Pcm: return "PCM";
    case CodecId::Mp3: return "MP3";
    case CodecId::Unsupported: return "unsupported";
    default: return "none";
    }
}
