/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "medf_es.h"

namespace {

class EsAudioDemuxer : public Demuxer {
public:
    ~EsAudioDemuxer() override { close(); }

    bool open(const std::string &path, const medf_arena_t &arena, bool want_cover) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us, int64_t *landed_us) override;

private:
    medf_es_demux_t *demux_ = nullptr;
};

bool EsAudioDemuxer::open(const std::string &path, const medf_arena_t &arena, bool want_cover) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = medf_es_demux_open(path.c_str(), &arena, want_cover, &failure);
    if (!demux_) {
        error_ = failure ? failure : "cannot read this audio file";
        return false;
    }
    buffer_ = medf_es_demux_buffer(demux_);

    const medf_es_info_t *es = medf_es_demux_info(demux_);
    info_.audio.codec = es->codec == MEDF_ES_CODEC_AAC ? CodecId::Aac : CodecId::Mp3;
    info_.audio.sample_rate = es->sample_rate;
    info_.audio.bitrate_bps = es->bitrate_bps;
    info_.audio.channels = es->channels;
    info_.audio.bits = 16;
    info_.duration_us = es->duration_us;
    info_.seekable = true;
    demuxer_apply_tags(es->tags, &info_);
    return true;
}

void EsAudioDemuxer::close() {
    if (demux_) {
        medf_es_demux_close(demux_);
        demux_ = nullptr;
    }
    buffer_ = nullptr;
    info_ = {};
}

bool EsAudioDemuxer::read(bool, Packet *out) {
    if (!demux_) return false;

    medf_es_packet_t packet = {};
    if (!medf_es_demux_read(demux_, &packet)) return false;
    out->track = TrackType::Audio;
    out->pts_us = packet.pts_us;
    out->keyframe = true;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

bool EsAudioDemuxer::seek(int64_t pts_us, int64_t *landed_us) {
    return demux_ && medf_es_demux_seek(demux_, pts_us, landed_us);
}

}

std::unique_ptr<Demuxer> es_audio_demuxer_create() {
    return std::make_unique<EsAudioDemuxer>();
}
