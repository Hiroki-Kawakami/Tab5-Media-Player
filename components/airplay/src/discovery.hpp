/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace airplay {

using TxtRecord = std::vector<std::pair<std::string, std::string>>;

/* Withdraws the service when destroyed. */
class Advertiser {
public:
    virtual ~Advertiser() = default;
    static std::unique_ptr<Advertiser> create(const std::string &host, const std::string &instance,
                                              const char *type, uint16_t port,
                                              const TxtRecord &txt);
};

/* Blocks up to timeout_ms; 0 when not found. Needs a live Advertiser on the device. */
uint16_t resolve_port(const std::string &instance, const char *type, uint32_t timeout_ms);

}  // namespace airplay
