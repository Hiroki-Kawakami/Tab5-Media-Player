/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RIFF_AUDIO_CODEC_NONE,
    RIFF_AUDIO_CODEC_PCM,
    RIFF_AUDIO_CODEC_MP3,
    RIFF_AUDIO_CODEC_ADPCM_IMA,
    RIFF_AUDIO_CODEC_AAC,
    RIFF_AUDIO_CODEC_UNSUPPORTED,
} riff_audio_codec_t;

const char *riff_audio_codec_name(riff_audio_codec_t codec);

#ifdef __cplusplus
}
#endif
