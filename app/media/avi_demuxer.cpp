/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "avi_demux.h"
#include "mpeg2_dec.h"
#include "esp_log.h"

static const char *TAG = "avi_demuxer";

namespace {

CodecId map_video(avi_video_codec_t codec) {
    switch (codec) {
    case AVI_VIDEO_CODEC_MJPEG: return CodecId::Mjpeg;
    case AVI_VIDEO_CODEC_H264: return CodecId::H264;
    case AVI_VIDEO_CODEC_MPEG2: return CodecId::Mpeg2;
    case AVI_VIDEO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

CodecId map_audio(riff_audio_codec_t codec) {
    switch (codec) {
    case RIFF_AUDIO_CODEC_PCM: return CodecId::Pcm;
    case RIFF_AUDIO_CODEC_MP3: return CodecId::Mp3;
    case RIFF_AUDIO_CODEC_ADPCM_IMA: return CodecId::AdpcmIma;
    case RIFF_AUDIO_CODEC_AAC: return CodecId::Aac;
    case RIFF_AUDIO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

class AviDemuxer : public Demuxer {
public:
    ~AviDemuxer() override { close(); }

    bool open(const std::string &path, const media_arena_t &arena, bool want_cover) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us, int64_t *landed_us) override;
    bool keyframeBefore(int64_t pts_us, int64_t *key_us) const override;

private:
    avi_demux_t *demux_ = nullptr;
    int64_t video_pts_us(const avi_packet_t &packet);

    int64_t interval_us_ = 0;
    int64_t next_video_pts_us_ = 0;
    int64_t gop_frame_ = -1;
};

bool AviDemuxer::open(const std::string &path, const media_arena_t &arena, bool want_cover) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = avi_demux_open(path.c_str(), &arena, want_cover, &failure);
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
    if (info_.video.codec == CodecId::H264 &&
        !h264_config_to_annexb(avi->video.codec_private, avi->video.codec_private_size,
                               &info_.video.codec_private, &info_.video.nal_length_size) &&
        avi->video.codec_private_size) {
        ESP_LOGW(TAG, "ignoring %u bytes of unrecognised H.264 extradata",
                 (unsigned)avi->video.codec_private_size);
    }
    if (info_.video.codec == CodecId::Mpeg2) {
        info_.video.codec_private.assign(avi->video.codec_private,
                                         avi->video.codec_private + avi->video.codec_private_size);
    }

    info_.audio.codec = map_audio(avi->audio.codec);
    info_.audio.sample_rate = avi->audio.sample_rate;
    info_.audio.bitrate_bps = avi->audio.bitrate_bps;
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
    demuxer_apply_tags(avi->tags, &info_);
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
    gop_frame_ = -1;
}

bool AviDemuxer::read(bool want_audio, Packet *out) {
    if (!demux_) return false;

    avi_packet_t packet = {};
    if (!avi_demux_read(demux_, &packet, want_audio)) return false;
    if (packet.type == AVI_PACKET_VIDEO) {
        out->track = TrackType::Video;
        out->pts_us = video_pts_us(packet);
        next_video_pts_us_ = (int64_t)(packet.frame_index + 1) * interval_us_;
    } else {
        out->track = TrackType::Audio;
        out->pts_us = next_video_pts_us_;
    }
    out->keyframe = packet.keyframe;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

int64_t AviDemuxer::video_pts_us(const avi_packet_t &packet) {
    const int64_t decode_order = (int64_t)packet.frame_index * interval_us_;
    if (info_.video.codec != CodecId::Mpeg2) return decode_order;
    mpeg2_dec_header_t header;
    if (!mpeg2_dec_header(packet.data, packet.size, &header)) return decode_order;
    if (header.gop) gop_frame_ = packet.frame_index;
    if (gop_frame_ < 0) return decode_order;
    return (gop_frame_ + header.temporal_reference) * interval_us_;
}

bool AviDemuxer::seek(int64_t pts_us, int64_t *landed_us) {
    if (!demux_ || interval_us_ <= 0) return false;
    const uint32_t frame = (uint32_t)(pts_us > 0 ? pts_us / interval_us_ : 0);
    uint32_t landed = 0;
    if (!avi_demux_seek(demux_, frame, &landed)) {
        ESP_LOGW(TAG, "cannot seek to frame %u", (unsigned)frame);
        return false;
    }
    next_video_pts_us_ = (int64_t)landed * interval_us_;
    gop_frame_ = -1;
    if (landed_us) *landed_us = next_video_pts_us_;
    return true;
}

bool AviDemuxer::keyframeBefore(int64_t pts_us, int64_t *key_us) const {
    if (!demux_ || interval_us_ <= 0 || pts_us < 0) return false;
    uint32_t key = 0;
    if (!avi_demux_keyframe_before(demux_, (uint32_t)(pts_us / interval_us_), &key)) return false;
    *key_us = (int64_t)key * interval_us_;
    return true;
}

}

std::unique_ptr<Demuxer> avi_demuxer_create() {
    return std::make_unique<AviDemuxer>();
}
