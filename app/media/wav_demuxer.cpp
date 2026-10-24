/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "medf_wav.h"

namespace {

CodecId map_audio(medf_riff_audio_codec_t codec) {
    switch (codec) {
    case MEDF_RIFF_AUDIO_CODEC_PCM: return CodecId::Pcm;
    case MEDF_RIFF_AUDIO_CODEC_MP3: return CodecId::Mp3;
    case MEDF_RIFF_AUDIO_CODEC_MP2: return CodecId::Mp2;
    case MEDF_RIFF_AUDIO_CODEC_ADPCM_IMA: return CodecId::AdpcmIma;
    case MEDF_RIFF_AUDIO_CODEC_AAC: return CodecId::Aac;
    case MEDF_RIFF_AUDIO_CODEC_NONE: return CodecId::None;
    default: return CodecId::Unsupported;
    }
}

class WavDemuxer : public Demuxer {
public:
    ~WavDemuxer() override { close(); }

    bool open(const std::string &path, const medf_arena_t &arena, bool want_cover) override;
    void close() override;
    bool isOpen() const override { return demux_ != nullptr; }
    bool read(bool want_audio, Packet *out) override;
    bool seek(int64_t pts_us, int64_t *landed_us) override;

private:
    medf_wav_demux_t *demux_ = nullptr;
};

bool WavDemuxer::open(const std::string &path, const medf_arena_t &arena, bool want_cover) {
    close();
    error_.clear();

    const char *failure = nullptr;
    demux_ = medf_wav_demux_open(path.c_str(), &arena, want_cover, &failure);
    if (!demux_) {
        error_ = failure ? failure : "cannot read this WAV";
        return false;
    }
    buffer_ = medf_wav_demux_buffer(demux_);

    const medf_wav_info_t *wav = medf_wav_demux_info(demux_);
    info_.audio.codec = map_audio(wav->codec);
    info_.audio.sample_rate = wav->sample_rate;
    info_.audio.bitrate_bps = wav->bitrate_bps;
    info_.audio.channels = wav->channels;
    info_.audio.bits = wav->bits_per_sample;
    info_.audio.block_align = wav->block_align;
    info_.audio.codec_private.assign(wav->codec_private,
                                     wav->codec_private + wav->codec_private_size);
    info_.duration_us = wav->duration_us;
    info_.seekable = true;
    demuxer_apply_tags(wav->tags, &info_);
    return true;
}

void WavDemuxer::close() {
    if (demux_) {
        medf_wav_demux_close(demux_);
        demux_ = nullptr;
    }
    buffer_ = nullptr;
    info_ = {};
}

bool WavDemuxer::read(bool, Packet *out) {
    if (!demux_) return false;

    medf_wav_packet_t packet = {};
    if (!medf_wav_demux_read(demux_, &packet)) return false;
    out->track = TrackType::Audio;
    out->pts_us = packet.pts_us;
    out->keyframe = true;
    out->data = packet.data;
    out->len = packet.size;
    out->ref = packet.ref;
    return true;
}

bool WavDemuxer::seek(int64_t pts_us, int64_t *landed_us) {
    return demux_ && medf_wav_demux_seek(demux_, pts_us, landed_us);
}

}

std::unique_ptr<Demuxer> wav_demuxer_create() {
    return std::make_unique<WavDemuxer>();
}
