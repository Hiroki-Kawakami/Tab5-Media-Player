/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>

namespace airplay {

enum class Codec {
    Pcm,
    Alac,
};

struct Format {
    Codec codec = Codec::Alac;
    uint32_t frames_per_packet = 352;
    uint32_t rate = 44100;
    uint8_t channels = 2;
    uint8_t alac_cookie[24] = {};
};

class Decoder {
public:
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    ~Decoder();

    bool open(const Format &format);
    /* Interleaved 16-bit output; returns frames, 0 on error. */
    uint32_t decode(const uint8_t *data, std::size_t len, int16_t *out, uint32_t max_frames);

private:
    Format format_;
    void *handle_ = nullptr;
};

}  // namespace airplay
