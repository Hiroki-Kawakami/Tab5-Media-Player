/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "bsp_types.h"
#include "media/psram_allocator.hpp"

enum class CodecId {
    None,
    Mjpeg,
    H264,
    Mpeg2,
    Pcm,
    Mp3,
    AdpcmIma,
    Aac,
    Opus,
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
    uint32_t bitrate_bps = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    uint16_t block_align = 0;
    std::vector<uint8_t> codec_private;
    uint8_t nal_length_size = 0;
    uint32_t max_packet_bytes = 0;
    bsp_rotation_t rotation = BSP_ROTATION_0;
};

struct MediaTags {
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string track;
    std::string date;

    bool empty() const {
        return title.empty() && artist.empty() && album.empty() && album_artist.empty() &&
               track.empty() && date.empty();
    }
};

enum class CoverFormat {
    None,
    Jpeg,
    Png,
};

using CoverBytes = PsramVector<uint8_t>;

struct CoverArt {
    std::shared_ptr<const CoverBytes> data;
    CoverFormat format = CoverFormat::None;

    explicit operator bool() const { return data && !data->empty(); }
};

struct MediaInfo {
    TrackInfo video;
    TrackInfo audio;
    int64_t duration_us = 0;
    int64_t frame_interval_us = 0;
    bool seekable = false;
    MediaTags tags;
    CoverArt cover;
};

struct MediaSummary {
    bool valid = false;
    const char *container = "";
    int64_t file_bytes = 0;
    int64_t duration_us = 0;
    bool seekable = false;
    MediaTags tags;
    CoverArt cover;
    struct {
        CodecId codec = CodecId::None;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t frame_interval_us = 0;
        bsp_rotation_t rotation = BSP_ROTATION_0;
        uint8_t profile_idc = 0;
        uint8_t level_idc = 0;
    } video;
    struct {
        CodecId codec = CodecId::None;
        uint32_t sample_rate = 0;
        uint32_t bitrate_bps = 0;
        uint8_t channels = 0;
        uint8_t bits = 0;
        std::string note;
    } audio;
};

struct Packet {
    TrackType track = TrackType::Video;
    int64_t pts_us = 0;
    bool keyframe = false;
    const uint8_t *data = nullptr;
    std::size_t len = 0;
    uint32_t ref = 0;
};

inline const char *codec_name(CodecId codec) {
    switch (codec) {
    case CodecId::Mjpeg: return "MJPEG";
    case CodecId::H264: return "H.264";
    case CodecId::Mpeg2: return "MPEG-2";
    case CodecId::Pcm: return "PCM";
    case CodecId::Mp3: return "MP3";
    case CodecId::AdpcmIma: return "IMA ADPCM";
    case CodecId::Aac: return "AAC";
    case CodecId::Opus: return "Opus";
    case CodecId::Unsupported: return "unsupported";
    default: return "none";
    }
}
