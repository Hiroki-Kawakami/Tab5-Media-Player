/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "avi_demux.h"
#include "esp_log.h"

static const char *TAG = "avi_demuxer";

namespace {

CodecId map_video(avi_video_codec_t codec) {
    switch (codec) {
    case AVI_VIDEO_CODEC_MJPEG: return CodecId::Mjpeg;
    case AVI_VIDEO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

CodecId map_audio(avi_audio_codec_t codec) {
    switch (codec) {
    case AVI_AUDIO_CODEC_PCM: return CodecId::Pcm;
    case AVI_AUDIO_CODEC_MP3: return CodecId::Mp3;
    case AVI_AUDIO_CODEC_ADPCM_IMA: return CodecId::AdpcmIma;
    case AVI_AUDIO_CODEC_AAC: return CodecId::Aac;
    case AVI_AUDIO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

class AviDemuxer : public Demuxer {
public:
    ~AviDemuxer() override { close(); }

    bool open(const std::string &path, const media_arena_t &arena) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us) override;

private:
    avi_demux_t *demux_ = nullptr;
    int64_t interval_us_ = 0;
    int64_t next_video_pts_us_ = 0;
};

bool AviDemuxer::open(const std::string &path, const media_arena_t &arena) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = avi_demux_open(path.c_str(), &arena, &failure);
    if (!demux_) {
        error_ = failure ? failure : "cannot read this AVI";
        return false;
    }
    buffer_ = avi_demux_buffer(demux_);

    const avi_info_t *avi = avi_demux_info(demux_);
    interval_us_ = avi->video.frame_interval_us;

    info_.video.codec = map_video(avi->video.codec);
    info_.video.width = avi->video.width;
    info_.video.height = avi->video.height;
    info_.video.max_packet_bytes = avi->video.max_frame_bytes;

    info_.audio.codec = map_audio(avi->audio.codec);
    info_.audio.sample_rate = avi->audio.sample_rate;
    info_.audio.channels = avi->audio.channels;
    info_.audio.bits = avi->audio.bits_per_sample;
    info_.audio.block_align = avi->audio.block_align;
    info_.audio.codec_private.assign(avi->audio.codec_private,
                                     avi->audio.codec_private + avi->audio.codec_private_size);
    info_.audio.max_packet_bytes = avi->audio.max_frame_bytes;
    if (info_.audio.codec != CodecId::None &&
        (info_.audio.sample_rate == 0 || info_.audio.channels == 0)) {
        info_.audio.codec = CodecId::Unsupported;
    }

    info_.frame_interval_us = interval_us_;
    info_.duration_us = (int64_t)avi->video.frame_count * interval_us_;
    info_.seekable = avi->seekable;
    next_video_pts_us_ = 0;
    return true;
}

void AviDemuxer::close() {
    if (demux_) {
        avi_demux_close(demux_);
        demux_ = nullptr;
    }
    buffer_ = nullptr;
    info_ = {};
    interval_us_ = 0;
    next_video_pts_us_ = 0;
}

bool AviDemuxer::read(bool want_audio, Packet *out) {
    if (!demux_) return false;

    avi_packet_t packet = {};
    if (!avi_demux_read(demux_, &packet, want_audio)) return false;
    if (packet.type == AVI_PACKET_VIDEO) {
        out->track = TrackType::Video;
        out->pts_us = (int64_t)packet.frame_index * interval_us_;
        next_video_pts_us_ = out->pts_us + interval_us_;
    } else {
        out->track = TrackType::Audio;
        out->pts_us = next_video_pts_us_;
    }
    out->keyframe = true;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

bool AviDemuxer::seek(int64_t pts_us) {
    if (!demux_ || interval_us_ <= 0) return false;
    const uint32_t frame = (uint32_t)(pts_us > 0 ? pts_us / interval_us_ : 0);
    if (!avi_demux_seek(demux_, frame)) {
        ESP_LOGW(TAG, "cannot seek to frame %u", (unsigned)frame);
        return false;
    }
    next_video_pts_us_ = (int64_t)frame * interval_us_;
    return true;
}

}

std::unique_ptr<Demuxer> avi_demuxer_create() {
    return std::make_unique<AviDemuxer>();
}
