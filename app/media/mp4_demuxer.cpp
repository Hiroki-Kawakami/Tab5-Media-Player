/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "mp4_demux.h"
#include "esp_log.h"

static const char *TAG = "mp4_demuxer";

namespace {

CodecId map_video(mp4_video_codec_t codec) {
    switch (codec) {
    case MP4_VIDEO_CODEC_MJPEG: return CodecId::Mjpeg;
    case MP4_VIDEO_CODEC_H264: return CodecId::H264;
    case MP4_VIDEO_CODEC_MPEG2: return CodecId::Mpeg2;
    case MP4_VIDEO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

CodecId map_audio(mp4_audio_codec_t codec) {
    switch (codec) {
    case MP4_AUDIO_CODEC_PCM: return CodecId::Pcm;
    case MP4_AUDIO_CODEC_MP3: return CodecId::Mp3;
    case MP4_AUDIO_CODEC_AAC: return CodecId::Aac;
    case MP4_AUDIO_CODEC_OPUS: return CodecId::Opus;
    case MP4_AUDIO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

bsp_rotation_t map_rotation(uint16_t degrees) {
    switch (degrees) {
    case 90:  return BSP_ROTATION_90;
    case 180: return BSP_ROTATION_180;
    case 270: return BSP_ROTATION_270;
    default:  return BSP_ROTATION_0;
    }
}

class Mp4Demuxer : public Demuxer {
public:
    ~Mp4Demuxer() override { close(); }

    bool open(const std::string &path, const media_arena_t &arena) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us, int64_t *landed_us) override;
    bool keyframeBefore(int64_t pts_us, int64_t *key_us) const override;

private:
    mp4_demux_t *demux_ = nullptr;
};

bool Mp4Demuxer::open(const std::string &path, const media_arena_t &arena) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = mp4_demux_open(path.c_str(), &arena, &failure);
    if (!demux_) {
        error_ = failure ? failure : "cannot read this MP4";
        return false;
    }
    buffer_ = mp4_demux_buffer(demux_);

    const mp4_info_t *mp4 = mp4_demux_info(demux_);
    info_.video.codec = map_video(mp4->video.codec);
    info_.video.width = mp4->video.width;
    info_.video.height = mp4->video.height;
    info_.video.rotation = map_rotation(mp4->video.rotation_ccw);
    if (info_.video.codec == CodecId::H264 &&
        !h264_config_to_annexb(mp4->video.codec_private, mp4->video.codec_private_size,
                               &info_.video.codec_private, &info_.video.nal_length_size)) {
        ESP_LOGW(TAG, "no usable avcC; assuming 4-byte NAL lengths");
        info_.video.nal_length_size = 4;
    }
    if (info_.video.codec == CodecId::Mpeg2) {
        info_.video.codec_private.assign(mp4->video.codec_private,
                                         mp4->video.codec_private + mp4->video.codec_private_size);
    }

    info_.audio.codec = map_audio(mp4->audio.codec);
    info_.audio.sample_rate = mp4->audio.sample_rate;
    info_.audio.bitrate_bps = mp4->audio.bitrate_bps;
    info_.audio.channels = mp4->audio.channels;
    info_.audio.bits = mp4->audio.bits_per_sample;
    info_.audio.codec_private.assign(mp4->audio.codec_private,
                                     mp4->audio.codec_private + mp4->audio.codec_private_size);

    info_.frame_interval_us = mp4->video.frame_interval_us;
    info_.duration_us = mp4->duration_us;
    info_.seekable = mp4->seekable;
    return true;
}

void Mp4Demuxer::close() {
    if (demux_) {
        mp4_demux_close(demux_);
        demux_ = nullptr;
    }
    buffer_ = nullptr;
    info_ = {};
}

bool Mp4Demuxer::read(bool want_audio, Packet *out) {
    if (!demux_) return false;

    mp4_packet_t packet = {};
    if (!mp4_demux_read(demux_, &packet, want_audio)) return false;
    out->track = packet.type == MP4_PACKET_VIDEO ? TrackType::Video : TrackType::Audio;
    out->pts_us = packet.pts_us;
    out->keyframe = packet.keyframe;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

bool Mp4Demuxer::seek(int64_t pts_us, int64_t *landed_us) {
    return demux_ && mp4_demux_seek(demux_, pts_us, landed_us);
}

bool Mp4Demuxer::keyframeBefore(int64_t pts_us, int64_t *key_us) const {
    return demux_ && mp4_demux_keyframe_before(demux_, pts_us, key_us);
}

}

std::unique_ptr<Demuxer> mp4_demuxer_create() {
    return std::make_unique<Mp4Demuxer>();
}
