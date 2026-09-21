/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "mkv_demux.h"
#include "esp_log.h"

static const char *TAG = "mkv_demuxer";

namespace {

CodecId map_video(mkv_video_codec_t codec) {
    switch (codec) {
    case MKV_VIDEO_CODEC_MJPEG: return CodecId::Mjpeg;
    case MKV_VIDEO_CODEC_H264: return CodecId::H264;
    case MKV_VIDEO_CODEC_MPEG2: return CodecId::Mpeg2;
    case MKV_VIDEO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

CodecId map_audio(mkv_audio_codec_t codec) {
    switch (codec) {
    case MKV_AUDIO_CODEC_PCM: return CodecId::Pcm;
    case MKV_AUDIO_CODEC_MP3: return CodecId::Mp3;
    case MKV_AUDIO_CODEC_ADPCM_IMA: return CodecId::AdpcmIma;
    case MKV_AUDIO_CODEC_AAC: return CodecId::Aac;
    case MKV_AUDIO_CODEC_OPUS: return CodecId::Opus;
    case MKV_AUDIO_CODEC_NONE: return CodecId::None;
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

class MkvDemuxer : public Demuxer {
public:
    ~MkvDemuxer() override { close(); }

    bool open(const std::string &path, const media_arena_t &arena) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us, int64_t *landed_us) override;
    bool keyframeBefore(int64_t pts_us, int64_t *key_us) const override;

private:
    mkv_demux_t *demux_ = nullptr;
};

bool MkvDemuxer::open(const std::string &path, const media_arena_t &arena) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = mkv_demux_open(path.c_str(), &arena, &failure);
    if (!demux_) {
        error_ = failure ? failure : "cannot read this MKV";
        return false;
    }
    buffer_ = mkv_demux_buffer(demux_);

    const mkv_info_t *mkv = mkv_demux_info(demux_);
    info_.video.codec = map_video(mkv->video.codec);
    info_.video.width = mkv->video.width;
    info_.video.height = mkv->video.height;
    info_.video.rotation = map_rotation(mkv->video.rotation_ccw);
    if (info_.video.codec == CodecId::H264 &&
        !h264_config_to_annexb(mkv->video.codec_private, mkv->video.codec_private_size,
                               &info_.video.codec_private, &info_.video.nal_length_size)) {
        ESP_LOGW(TAG, "no usable avcC in CodecPrivate; assuming 4-byte NAL lengths");
        info_.video.nal_length_size = 4;
    }
    if (info_.video.codec == CodecId::Mpeg2) {
        info_.video.codec_private.assign(mkv->video.codec_private,
                                         mkv->video.codec_private + mkv->video.codec_private_size);
    }

    info_.audio.codec = map_audio(mkv->audio.codec);
    info_.audio.sample_rate = mkv->audio.sample_rate;
    info_.audio.channels = mkv->audio.channels;
    info_.audio.bits = mkv->audio.bits_per_sample;
    info_.audio.block_align = mkv->audio.block_align;
    info_.audio.codec_private.assign(mkv->audio.codec_private,
                                     mkv->audio.codec_private + mkv->audio.codec_private_size);

    info_.frame_interval_us = mkv->video.frame_interval_us;
    info_.duration_us = mkv->duration_us;
    info_.seekable = mkv->seekable;
    demuxer_apply_tags(mkv->tags, &info_);
    return true;
}

void MkvDemuxer::close() {
    if (demux_) {
        mkv_demux_close(demux_);
        demux_ = nullptr;
    }
    buffer_ = nullptr;
    info_ = {};
}

bool MkvDemuxer::read(bool want_audio, Packet *out) {
    if (!demux_) return false;

    mkv_packet_t packet = {};
    if (!mkv_demux_read(demux_, &packet, want_audio)) return false;
    out->track = packet.type == MKV_PACKET_VIDEO ? TrackType::Video : TrackType::Audio;
    out->pts_us = packet.pts_us;
    out->keyframe = packet.keyframe;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

bool MkvDemuxer::seek(int64_t pts_us, int64_t *landed_us) {
    return demux_ && mkv_demux_seek(demux_, pts_us, landed_us);
}

bool MkvDemuxer::keyframeBefore(int64_t pts_us, int64_t *key_us) const {
    return demux_ && mkv_demux_keyframe_before(demux_, pts_us, key_us);
}

}

std::unique_ptr<Demuxer> mkv_demuxer_create() {
    return std::make_unique<MkvDemuxer>();
}
