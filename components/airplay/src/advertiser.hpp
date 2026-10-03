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

}  // namespace airplay
