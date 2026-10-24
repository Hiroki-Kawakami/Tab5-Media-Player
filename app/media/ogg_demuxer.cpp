/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "medf_ogg.h"

namespace {

class OggDemuxer : public Demuxer {
public:
    ~OggDemuxer() override { close(); }

    bool open(const std::string &path, const medf_arena_t &arena, bool want_cover) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us, int64_t *landed_us) override;
    void release(uint32_t ref) override { medf_ogg_demux_release(demux_, ref); }
    void releaseAll() override { medf_ogg_demux_release_all(demux_); }

private:
    medf_ogg_demux_t *demux_ = nullptr;
};

bool OggDemuxer::open(const std::string &path, const medf_arena_t &arena, bool want_cover) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = medf_ogg_demux_open(path.c_str(), &arena, want_cover, &failure);
    if (!demux_) {
        error_ = failure ? failure : "cannot read this Ogg file";
        return false;
    }
    buffer_ = medf_ogg_demux_buffer(demux_);

    const medf_ogg_info_t *ogg = medf_ogg_demux_info(demux_);
    info_.audio.codec = CodecId::Opus;
    info_.audio.sample_rate = ogg->sample_rate;
    info_.audio.bitrate_bps = ogg->bitrate_bps;
    info_.audio.channels = ogg->channels;
    info_.audio.codec_private.assign(ogg->opus_head, ogg->opus_head + sizeof(ogg->opus_head));
    info_.duration_us = ogg->duration_us;
    info_.seekable = ogg->duration_us > 0;
    demuxer_apply_tags(ogg->tags, &info_);
    return true;
}

void OggDemuxer::close() {
    if (demux_) {
        medf_ogg_demux_close(demux_);
        demux_ = nullptr;
    }
    buffer_ = nullptr;
    info_ = {};
}

bool OggDemuxer::read(bool, Packet *out) {
    if (!demux_) return false;

    medf_ogg_packet_t packet = {};
    if (!medf_ogg_demux_read(demux_, &packet)) return false;
    out->track = TrackType::Audio;
    out->pts_us = packet.pts_us;
    out->keyframe = true;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

bool OggDemuxer::seek(int64_t pts_us, int64_t *landed_us) {
    return demux_ && medf_ogg_demux_seek(demux_, pts_us, landed_us);
}

}

std::unique_ptr<Demuxer> ogg_demuxer_create() {
    return std::make_unique<OggDemuxer>();
}
