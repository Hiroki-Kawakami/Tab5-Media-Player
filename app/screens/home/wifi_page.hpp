/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "home_page.hpp"
#include "media/psram_allocator.hpp"
#include "wifi_manager.hpp"

class WifiPage : public HomePage,
                 public wifi::Listener,
                 public std::enable_shared_from_this<WifiPage> {
public:
    const std::string &title() const override { return title_; }
    void build_navigation(lv_obj_t *navigation) override;
    void build(lv_obj_t *contents) override;
    void save_state() override;
    void on_wifi_state(const wifi::Status &status) override;

private:
    struct Network {
        PsramString ssid;
        int8_t rssi;
        bool secured;
    };

    const std::string title_ = "Wi-Fi";
    PsramVector<Network> networks_;
    uint32_t scan_token_ = 0;
    bool scanned_ = false;
    bool busy_ = false;
    bool scanning_ = false;
    bool scan_pending_ = false;

    lv_obj_t *switch_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *ip_row_ = nullptr;
    lv_obj_t *ip_value_ = nullptr;
    lv_obj_t *mac_row_ = nullptr;
    lv_obj_t *mac_value_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *password_modal_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;
    lv_obj_t *connecting_modal_ = nullptr;

    static void post(std::weak_ptr<WifiPage> weak, std::function<void(WifiPage &)> fn);
    void set_enabled(bool enabled);
    void maybe_scan();
    void start_scan();
    void update_status();
    void rebuild_list();
    void select(const std::string &ssid, bool secured);
    void open_password(const std::string &ssid);
    void close_password();
    void connect(const std::string &ssid, const std::string &password);
};
