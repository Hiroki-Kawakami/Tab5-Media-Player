/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "discovery.hpp"
#include "esp_log.h"
#include "mdns.h"

namespace airplay {

static const char *TAG = "airplay";

class MdnsAdvertiser : public Advertiser {
public:
    ~MdnsAdvertiser() override { mdns_free(); }
};

std::unique_ptr<Advertiser> Advertiser::create(const std::string &host, const std::string &instance,
                                               const char *type, uint16_t port,
                                               const TxtRecord &txt) {
    const std::string service(type);
    const std::size_t dot = service.find('.');
    if (dot == std::string::npos) return nullptr;
    const std::string name = service.substr(0, dot);
    const std::string protocol = service.substr(dot + 1);

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return nullptr;
    }
    auto advertiser = std::make_unique<MdnsAdvertiser>();
    std::vector<mdns_txt_item_t> items;
    items.reserve(txt.size());
    for (const auto &[key, value] : txt) items.push_back({ key.c_str(), value.c_str() });

    err = mdns_hostname_set(host.c_str());
    if (err == ESP_OK) {
        err = mdns_service_add(instance.c_str(), name.c_str(), protocol.c_str(), port,
                               items.data(), items.size());
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns: %s", esp_err_to_name(err));
        return nullptr;
    }
    return advertiser;
}

uint16_t resolve_port(const std::string &instance, const char *type, uint32_t timeout_ms) {
    const std::string service(type);
    const std::size_t dot = service.find('.');
    if (dot == std::string::npos) return 0;
    mdns_result_t *results = nullptr;
    if (mdns_query_srv(instance.c_str(), service.substr(0, dot).c_str(),
                       service.substr(dot + 1).c_str(), timeout_ms, &results) != ESP_OK) {
        return 0;
    }
    const uint16_t port = results ? results->port : 0;
    mdns_query_results_free(results);
    return port;
}

}  // namespace airplay
