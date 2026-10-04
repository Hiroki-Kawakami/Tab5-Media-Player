/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "discovery.hpp"
#include "esp_log.h"

#include <arpa/inet.h>
#include <sys/select.h>
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

static void resolved(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType err,
                     const char *, const char *, uint16_t port, uint16_t, const unsigned char *,
                     void *context) {
    if (err == kDNSServiceErr_NoError) *static_cast<uint16_t *>(context) = ntohs(port);
}

uint16_t resolve_port(const std::string &instance, const char *type, uint32_t timeout_ms) {
    uint16_t port = 0;
    DNSServiceRef ref = nullptr;
    if (DNSServiceResolve(&ref, 0, 0, instance.c_str(), type, "local.", resolved, &port) !=
        kDNSServiceErr_NoError) {
        return 0;
    }
    const int fd = DNSServiceRefSockFD(ref);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeval timeout = { (time_t)(timeout_ms / 1000), (suseconds_t)(timeout_ms % 1000 * 1000) };
    if (select(fd + 1, &readable, nullptr, nullptr, &timeout) > 0) DNSServiceProcessResult(ref);
    DNSServiceRefDeallocate(ref);
    return port;
}

}  // namespace airplay
