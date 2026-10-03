/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "psa/crypto.h"

namespace airplay {

std::string base64_encode(const uint8_t *data, std::size_t len);
/* Accepts missing padding and skips whitespace. */
std::vector<uint8_t> base64_decode(std::string_view text);

class AirportKey {
public:
    AirportKey() = default;
    AirportKey(const AirportKey &) = delete;
    AirportKey &operator=(const AirportKey &) = delete;
    ~AirportKey();

    bool sign_challenge(const uint8_t *data, std::size_t len, std::vector<uint8_t> *signature);
    bool decrypt_key(const std::vector<uint8_t> &wrapped, std::vector<uint8_t> *key);

private:
    psa_key_id_t sign_ = 0;
    psa_key_id_t decrypt_ = 0;
};

class AesCbc {
public:
    AesCbc() = default;
    AesCbc(const AesCbc &) = delete;
    AesCbc &operator=(const AesCbc &) = delete;
    ~AesCbc();

    bool init(const uint8_t key[16], const uint8_t iv[16]);
    /* Whole blocks only; the trailing partial block of a packet is sent in the clear. */
    bool decrypt(const uint8_t *in, uint8_t *out, std::size_t len);

private:
    psa_key_id_t key_ = 0;
    uint8_t iv_[16] = {};
};

}  // namespace airplay
