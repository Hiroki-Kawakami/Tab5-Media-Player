/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "crypto.hpp"
#include "airport_key.hpp"

namespace airplay {

static constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t *data, std::size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        uint32_t chunk = (uint32_t)data[i] << 16;
        if (i + 1 < len) chunk |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) chunk |= data[i + 2];
        out += kAlphabet[(chunk >> 18) & 63];
        out += kAlphabet[(chunk >> 12) & 63];
        out += i + 1 < len ? kAlphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < len ? kAlphabet[chunk & 63] : '=';
    }
    return out;
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> base64_decode(std::string_view text) {
    std::vector<uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        if (c == '=') break;
        const int value = base64_value(c);
        if (value < 0) continue;
        bits = (bits << 6) | (uint32_t)value;
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back((uint8_t)(bits >> count));
        }
    }
    return out;
}

static psa_key_id_t import_rsa(psa_key_usage_t usage, psa_algorithm_t algorithm) {
    if (psa_crypto_init() != PSA_SUCCESS) return 0;
    const std::vector<uint8_t> der = base64_decode(kAirportKeyBase64);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, algorithm);
    psa_key_id_t key = 0;
    if (psa_import_key(&attributes, der.data(), der.size(), &key) != PSA_SUCCESS) return 0;
    return key;
}

AirportKey::~AirportKey() {
    psa_destroy_key(sign_);
    psa_destroy_key(decrypt_);
}

bool AirportKey::sign_challenge(const uint8_t *data, std::size_t len,
                                std::vector<uint8_t> *signature) {
    if (!sign_) {
        sign_ = import_rsa(PSA_KEY_USAGE_SIGN_HASH, PSA_ALG_RSA_PKCS1V15_SIGN_RAW);
        if (!sign_) return false;
    }
    signature->resize(256);
    std::size_t written = 0;
    if (psa_sign_hash(sign_, PSA_ALG_RSA_PKCS1V15_SIGN_RAW, data, len, signature->data(),
                      signature->size(), &written) != PSA_SUCCESS) {
        return false;
    }
    signature->resize(written);
    return true;
}

bool AirportKey::decrypt_key(const std::vector<uint8_t> &wrapped, std::vector<uint8_t> *key) {
    const psa_algorithm_t algorithm = PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1);
    if (!decrypt_) {
        decrypt_ = import_rsa(PSA_KEY_USAGE_DECRYPT, algorithm);
        if (!decrypt_) return false;
    }
    key->resize(256);
    std::size_t written = 0;
    if (psa_asymmetric_decrypt(decrypt_, algorithm, wrapped.data(), wrapped.size(), nullptr,
                               0, key->data(), key->size(), &written) != PSA_SUCCESS) {
        return false;
    }
    key->resize(written);
    return true;
}

AesCbc::~AesCbc() { psa_destroy_key(key_); }

bool AesCbc::init(const uint8_t key[16], const uint8_t iv[16]) {
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CBC_NO_PADDING);
    if (psa_import_key(&attributes, key, 16, &key_) != PSA_SUCCESS) return false;
    for (int i = 0; i < 16; i++) iv_[i] = iv[i];
    return true;
}

bool AesCbc::decrypt(const uint8_t *in, uint8_t *out, std::size_t len) {
    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    std::size_t done = 0;
    std::size_t tail = 0;
    const bool ok = psa_cipher_decrypt_setup(&operation, key_, PSA_ALG_CBC_NO_PADDING) == PSA_SUCCESS &&
                    psa_cipher_set_iv(&operation, iv_, sizeof(iv_)) == PSA_SUCCESS &&
                    psa_cipher_update(&operation, in, len, out, len, &done) == PSA_SUCCESS &&
                    psa_cipher_finish(&operation, out + done, len - done, &tail) == PSA_SUCCESS;
    psa_cipher_abort(&operation);
    return ok && done + tail == len;
}

}  // namespace airplay
