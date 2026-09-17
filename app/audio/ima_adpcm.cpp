/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "ima_adpcm.hpp"
#include <algorithm>

namespace {

constexpr int16_t kSteps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

constexpr std::size_t kGroupBytes = 4;

constexpr int8_t kIndexAdjust[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

struct ImaChannel {
    int predictor;
    int index;
};

int16_t ima_step(ImaChannel &channel, uint8_t nibble) {
    const int step = kSteps[channel.index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    channel.predictor = std::clamp(channel.predictor + diff, -32768, 32767);
    channel.index = std::clamp(channel.index + kIndexAdjust[nibble], 0, 88);
    return (int16_t)channel.predictor;
}

}

std::size_t ima_adpcm_decode(const uint8_t *block, std::size_t len, uint8_t channels,
                             int16_t *out, std::size_t max_frames) {
    const std::size_t header = 4u * channels;
    if (channels == 0 || channels > kImaAdpcmMaxChannels || len < header) return 0;
    const std::size_t groups = (len - header) / (kGroupBytes * channels);
    const std::size_t frames = 1 + groups * kGroupBytes * 2;
    if (frames > max_frames) return 0;

    ImaChannel state[kImaAdpcmMaxChannels] = {};
    for (uint8_t c = 0; c < channels; c++) {
        const uint8_t *head = block + 4u * c;
        state[c].predictor = (int16_t)(head[0] | (head[1] << 8));
        state[c].index = std::min<int>(head[2], 88);
        out[c] = (int16_t)state[c].predictor;
    }

    const uint8_t *p = block + header;
    for (std::size_t g = 0; g < groups; g++) {
        for (uint8_t c = 0; c < channels; c++) {
            int16_t *samples = out + (1 + g * kGroupBytes * 2) * channels + c;
            for (std::size_t k = 0; k < kGroupBytes; k++) {
                const uint8_t byte = *p++;
                samples[0] = ima_step(state[c], byte & 0x0F);
                samples[channels] = ima_step(state[c], byte >> 4);
                samples += 2 * channels;
            }
        }
    }
    return frames;
}
