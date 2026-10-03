/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "advertiser.hpp"
#include "esp_log.h"

#include <arpa/inet.h>
#include <dns_sd.h>

namespace airplay {

static const char *TAG = "airplay";

class DnsSdAdvertiser : public Advertiser {
public:
    explicit DnsSdAdvertiser(DNSServiceRef ref) : ref_(ref) {}
    ~DnsSdAdvertiser() override { DNSServiceRefDeallocate(ref_); }

private:
    DNSServiceRef ref_;
};

std::unique_ptr<Advertiser> Advertiser::create(const std::string &, const std::string &instance,
                                               const char *type, uint16_t port,
                                               const TxtRecord &txt) {
    std::string record;
    for (const auto &[key, value] : txt) {
        const std::string item = key + "=" + value;
        record += (char)item.size();
        record += item;
    }
    DNSServiceRef ref = nullptr;
    const DNSServiceErrorType err =
        DNSServiceRegister(&ref, 0, 0, instance.c_str(), type, nullptr, nullptr, htons(port),
                           (uint16_t)record.size(), record.data(), nullptr, nullptr);
    if (err != kDNSServiceErr_NoError) {
        ESP_LOGE(TAG, "DNSServiceRegister: %d", (int)err);
        return nullptr;
    }
    return std::make_unique<DnsSdAdvertiser>(ref);
}

}  // namespace airplay
