/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "medf_flac.h"

namespace {

class FlacDemuxer : public Demuxer {
public:
    ~FlacDemuxer() override { close(); }

    bool open(const std::string &path, const medf_arena_t &arena, bool want_cover) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us, int64_t *landed_us) override;

private:
    medf_flac_demux_t *demux_ = nullptr;
};

bool FlacDemuxer::open(const std::string &path, const medf_arena_t &arena, bool want_cover) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = medf_flac_demux_open(path.c_str(), &arena, want_cover, &failure);
    if (!demux_) {
        error_ = failure ? failure : "cannot read this FLAC file";
        return false;
    }
    buffer_ = medf_flac_demux_buffer(demux_);

    const medf_flac_info_t *flac = medf_flac_demux_info(demux_);
    info_.audio.codec = CodecId::Flac;
    info_.audio.sample_rate = flac->sample_rate;
    info_.audio.bitrate_bps = flac->bitrate_bps;
    info_.audio.channels = flac->channels;
    info_.audio.bits = flac->bits_per_sample;
    info_.audio.codec_private.assign(flac->streaminfo, flac->streaminfo + sizeof(flac->streaminfo));
    info_.duration_us = flac->duration_us;
    info_.seekable = flac->duration_us > 0;
    demuxer_apply_tags(flac->tags, &info_);
    return true;
}

void FlacDemuxer::close() {
    if (demux_) {
        medf_flac_demux_close(demux_);
        demux_ = nullptr;
    }
    buffer_ = nullptr;
    info_ = {};
}

bool FlacDemuxer::read(bool, Packet *out) {
    if (!demux_) return false;

    medf_flac_packet_t packet = {};
    if (!medf_flac_demux_read(demux_, &packet)) return false;
    out->track = TrackType::Audio;
    out->pts_us = packet.pts_us;
    out->keyframe = true;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

bool FlacDemuxer::seek(int64_t pts_us, int64_t *landed_us) {
    return demux_ && medf_flac_demux_seek(demux_, pts_us, landed_us);
}

}

std::unique_ptr<Demuxer> flac_demuxer_create() {
    return std::make_unique<FlacDemuxer>();
}
