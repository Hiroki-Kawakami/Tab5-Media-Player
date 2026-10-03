/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "wifi_page.hpp"

#include <algorithm>
#include <utility>
#include <vector>
#include "grouped_list.hpp"
#include "resources.h"
#include "settings.hpp"
#include "settings_widgets.hpp"
#include "widgets.hpp"

static constexpr int32_t kListRowHeight = 88;
static constexpr int32_t kPadding = 24;
static constexpr uint32_t kMutedColor = 0x808080;
static constexpr uint32_t kConnectedColor = 0x2196f3;
static constexpr uint32_t kOnlineColor = 0x2e7d32;

static const char *signal_icon(int8_t rssi) {
    if (rssi >= -55) return TABLER_WIFI;
    if (rssi >= -67) return TABLER_WIFI_2;
    if (rssi >= -78) return TABLER_WIFI_1;
    return TABLER_WIFI_0;
}

static const char *result_message(wifi::Result result) {
    switch (result) {
    case wifi::Result::ApNotFound:
        return "Network not found. It may be out of range.";
    case wifi::Result::AuthFailed:
        return "Wrong password, or authentication failed.";
    case wifi::Result::AssocFailed:
        return "Could not associate with the network.";
    case wifi::Result::IpFailed:
        return "Joined the network but could not get an IP address.";
    case wifi::Result::Timeout:
        return "The connection timed out.";
    default:
        return "Could not connect to the network.";
    }
}

static lv_obj_t *list_message_create(lv_obj_t *list, const char *text) {
    auto row = lv_container_create(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(row, kListRowHeight);
    lv_obj_set_style_pad_hor(row, kPadding, 0);
    lv_obj_set_style_pad_column(row, kPadding, 0);
    auto label = lv_label_create(row);
    lv_obj_set_font_role(label, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_color(label, lv_color_hex(kMutedColor), 0);
    lv_label_set_text(label, text);
    return row;
}

void WifiPage::post(std::weak_ptr<WifiPage> weak, std::function<void(WifiPage &)> fn) {
    lv_lock();
    lv_async_call([weak = std::move(weak), fn = std::move(fn)] {
        if (auto self = weak.lock()) fn(*self);
    });
    lv_unlock();
}

void WifiPage::build_navigation(lv_obj_t *navigation) {
    auto button = lv_button_create(lv_navigation_actions(navigation), LV_BUTTON_STYLE_NAVIGATION);
    lv_button_set_text(button, TABLER_REFRESH, &icon_36);
    lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this](lv_event_t *) { maybe_scan(); });
}

void WifiPage::build(lv_obj_t *contents) {
    lv_setting_page_style(contents);
    auto section = lv_setting_section_create(contents);

    auto row = lv_setting_row_create(section, "Wi-Fi");
    switch_ = lv_setting_switch_create(row, settings_wifi_enabled(),
                                       [this](lv_obj_t *, bool enabled) { set_enabled(enabled); });

    lv_hor_separator_create(section);

    status_ = lv_label_create(section);
    lv_obj_set_width(status_, LV_PCT(100));
    lv_obj_set_font_role(status_, LV_WIDGETS_FONT_BODY);

    ip_row_ = lv_setting_row_create(section, "IP Address");
    ip_value_ = lv_setting_value_create(ip_row_);
    mac_row_ = lv_setting_row_create(section, "MAC Address");
    mac_value_ = lv_setting_value_create(mac_row_);

    list_ = lv_grouped_section_create(contents);

    wifi::manager().set_listener(
        std::shared_ptr<wifi::Listener>(shared_from_this(), static_cast<wifi::Listener *>(this)));
    update_status();
    rebuild_list();
    if (!scanned_) maybe_scan();
}

void WifiPage::save_state() {
    switch_ = status_ = ip_row_ = ip_value_ = mac_row_ = mac_value_ = list_ = nullptr;
    password_modal_ = keyboard_ = connecting_modal_ = nullptr;
}

void WifiPage::on_wifi_state(const wifi::Status &) {
    post(weak_from_this(), [](WifiPage &page) {
        page.update_status();
        page.rebuild_list();
        if (page.scan_pending_ && wifi::manager().status().state != wifi::State::Connecting) {
            page.start_scan();
        } else if (!page.scanned_) {
            page.maybe_scan();
        }
    });
}

void WifiPage::set_enabled(bool enabled) {
    if (busy_) return;
    busy_ = true;
    scan_token_++;
    scan_pending_ = false;
    scanning_ = enabled;
    if (!enabled) networks_.clear();
    settings_set_wifi_enabled(enabled, [weak = weak_from_this(), enabled] {
        post(weak, [enabled](WifiPage &page) {
            page.busy_ = false;
            page.scanning_ = false;
            if (enabled) page.maybe_scan();
            page.update_status();
            page.rebuild_list();
        });
    });
    settings_commit();
    update_status();
    rebuild_list();
}

void WifiPage::maybe_scan() {
    if (busy_ || !wifi::manager().enabled()) return;
    if (wifi::manager().status().state == wifi::State::Connecting) {
        scan_pending_ = true;
        scanning_ = true;
        rebuild_list();
        return;
    }
    start_scan();
}

void WifiPage::start_scan() {
    scan_pending_ = false;
    scanning_ = true;
    scanned_ = true;
    rebuild_list();
    const uint32_t token = ++scan_token_;
    wifi::manager().scan([weak = weak_from_this(), token](wifi::Result, std::vector<wifi::AP> aps) {
        PsramVector<Network> networks;
        networks.reserve(aps.size());
        for (const auto &ap : aps) {
            networks.push_back({PsramString(ap.ssid.c_str()), ap.rssi, ap.secured});
        }
        post(weak, [token, networks = std::move(networks)](WifiPage &page) mutable {
            if (token != page.scan_token_) return;
            page.scanning_ = false;
            page.networks_ = std::move(networks);
            page.rebuild_list();
        });
    });
}

void WifiPage::update_status() {
    if (!status_) return;
    lv_obj_set_state(switch_, LV_STATE_CHECKED, settings_wifi_enabled());
    lv_obj_set_state(switch_, LV_STATE_DISABLED, busy_);

    const wifi::Status status = wifi::manager().status();
    uint32_t color = kMutedColor;
    switch (status.state) {
    case wifi::State::Off:
        lv_label_set_text(status_, settings_wifi_enabled() ? "Turning on Wi-Fi..." : "Wi-Fi off");
        break;
    case wifi::State::Connected:
        lv_label_set_text_fmt(status_, "Connected to %s", status.ssid.c_str());
        color = kOnlineColor;
        break;
    case wifi::State::Connecting:
        lv_label_set_text_fmt(status_, "Connecting to %s...", status.ssid.c_str());
        break;
    case wifi::State::Disconnected: {
        const std::string saved = wifi::manager().saved_ssid();
        if (saved.empty()) {
            lv_label_set_text(status_, "Not connected");
        } else {
            lv_label_set_text_fmt(status_, "Not connected (saved: %s)", saved.c_str());
        }
        break;
    }
    }
    lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);

    const bool has_ip = status.state == wifi::State::Connected && !status.ip.empty();
    lv_obj_set_flag(ip_row_, LV_OBJ_FLAG_HIDDEN, !has_ip);
    if (has_ip) lv_label_set_text(ip_value_, status.ip.c_str());

    const std::string mac = wifi::manager().mac_address();
    lv_obj_set_flag(mac_row_, LV_OBJ_FLAG_HIDDEN, mac.empty());
    if (!mac.empty()) lv_label_set_text(mac_value_, mac.c_str());
}

void WifiPage::rebuild_list() {
    if (!list_) return;
    lv_obj_clean(list_);
    lv_obj_set_flag(list_, LV_OBJ_FLAG_HIDDEN, !scanning_ && !wifi::manager().enabled());

    if (scanning_) {
        auto row = list_message_create(list_, "Searching for networks...");
        auto spinner = lv_spinner_create(row);
        lv_obj_set_size(spinner, 40, 40);
        lv_obj_move_to_index(spinner, 0);
        return;
    }
    if (networks_.empty()) {
        list_message_create(list_, "No networks found");
        return;
    }

    const wifi::Status status = wifi::manager().status();
    const std::string connected = status.state == wifi::State::Connected ? status.ssid : std::string();
    const std::string pinned = connected.empty() ? wifi::manager().saved_ssid() : connected;

    std::vector<const Network *> order;
    order.reserve(networks_.size());
    for (const auto &network : networks_) order.push_back(&network);
    std::stable_partition(order.begin(), order.end(), [&pinned](const Network *network) {
        return !pinned.empty() && pinned == network->ssid.c_str();
    });

    for (const Network *network : order) {
        std::string ssid(network->ssid.c_str());
        const bool secured = network->secured;
        auto row = lv_grouped_row_create(list_, signal_icon(network->rssi), ssid.c_str(), &icon_36);
        if (ssid == connected) {
            lv_obj_set_style_text_color(lv_obj_get_child(row, 0), lv_color_hex(kConnectedColor), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(row, 1), lv_color_hex(kConnectedColor), 0);
        }
        auto trailing = lv_obj_get_child(row, 2);
        if (secured) {
            lv_obj_set_style_text_font(trailing, &icon_36, 0);
            lv_label_set_text(trailing, TABLER_LOCK);
        } else {
            lv_obj_add_flag(trailing, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_event_fn(row, LV_EVENT_CLICKED, [this, ssid, secured](lv_event_t *) {
            select(ssid, secured);
        });
    }
}

void WifiPage::select(const std::string &ssid, bool secured) {
    if (busy_) return;
    if (secured) {
        open_password(ssid);
    } else {
        connect(ssid, "");
    }
}

void WifiPage::open_password(const std::string &ssid) {
    if (password_modal_) return;
    auto screen = lv_obj_get_screen(list_);
    password_modal_ = lv_modal_open(screen);
    lv_modal_title_create(password_modal_, ("Connect to " + ssid).c_str());

    auto textarea = lv_textarea_create(password_modal_);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, true);
    lv_textarea_set_placeholder_text(textarea, "Password");
    lv_obj_set_width(textarea, LV_PCT(100));
    lv_obj_set_font_role(textarea, LV_WIDGETS_FONT_BODY);

    auto buttons = lv_container_create(password_modal_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 16, 0);

    // Deleting the keyboard inside its own event hangs LVGL, so closing goes through post().
    auto weak = weak_from_this();
    auto cancel = [weak](lv_event_t *) { post(weak, [](WifiPage &page) { page.close_password(); }); };
    auto submit = [weak, textarea, ssid](lv_event_t *) {
        std::string password = lv_textarea_get_text(textarea);
        post(weak, [ssid, password](WifiPage &page) {
            page.close_password();
            if (page.list_) page.connect(ssid, password);
        });
    };
    lv_modal_button_create(buttons, "Cancel", LV_MODAL_BUTTON_TYPE_SECONDARY, cancel);
    lv_modal_button_create(buttons, "Connect", LV_MODAL_BUTTON_TYPE_PRIMARY, submit);

    keyboard_ = lv_keyboard_create(screen);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(keyboard_, LV_PCT(100), LV_PCT(40));
    lv_obj_set_font_role(keyboard_, LV_WIDGETS_FONT_BODY);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard_, textarea);
    lv_obj_add_event_fn(keyboard_, LV_EVENT_READY, submit);
    lv_obj_add_event_fn(keyboard_, LV_EVENT_CANCEL, cancel);

    lv_obj_update_layout(screen);
    const int32_t above = lv_obj_get_height(screen) - lv_obj_get_height(keyboard_);
    lv_obj_align(password_modal_, LV_ALIGN_TOP_MID, 0,
                 std::max<int32_t>(0, (above - lv_obj_get_height(password_modal_)) / 2));
}

void WifiPage::close_password() {
    if (keyboard_) lv_obj_delete(keyboard_);
    if (password_modal_) lv_modal_close(password_modal_);
    keyboard_ = password_modal_ = nullptr;
}

void WifiPage::connect(const std::string &ssid, const std::string &password) {
    connecting_modal_ = lv_modal_open(lv_obj_get_screen(list_));
    auto header = lv_container_create(connecting_modal_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(header, kPadding, 0);
    auto spinner = lv_spinner_create(header);
    lv_obj_set_size(spinner, 64, 64);
    lv_modal_title_create(header, ("Connecting to " + ssid + "...").c_str());

    wifi::manager().connect(ssid, password, [weak = weak_from_this()](wifi::Result result) {
        post(weak, [result](WifiPage &page) {
            if (page.connecting_modal_) lv_modal_close(page.connecting_modal_);
            page.connecting_modal_ = nullptr;
            if (result == wifi::Result::Ok || !page.list_) return;
            auto modal = lv_modal_open(lv_obj_get_screen(page.list_));
            lv_modal_title_create(modal, "Connection failed");
            lv_modal_message_create(modal, result_message(result));
            lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                                   [modal](lv_event_t *) { lv_modal_close(modal); });
        });
    });
}
