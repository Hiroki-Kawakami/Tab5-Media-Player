/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "remote.hpp"
#include "discovery.hpp"
#include "esp_log.h"
#include "esp_timer.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/time.h>
#include <unistd.h>

namespace airplay {

static const char *TAG = "airplay";

static constexpr uint32_t kStackBytes = 6144;
static constexpr uint32_t kPollMs = 50;
static constexpr uint32_t kResolveTimeoutMs = 1500;
static constexpr int64_t kResolveRetryUs = 3000000;
static constexpr int kTimeoutSeconds = 2;

Remote::~Remote() { stop(); }

void Remote::stop() {
    quit_ = true;
    task_.join();
}

bool Remote::start() {
    return task_.start("airplay_dacp", kStackBytes, 3, [this] { loop(); });
}

void Remote::set_target(const sockaddr_storage &peer, const std::string &dacp_id,
                        const std::string &active_remote) {
    std::lock_guard<std::mutex> guard(lock_);
    if (dacp_id == dacp_id_ && active_remote == active_remote_) return;
    peer_ = peer;
    dacp_id_ = dacp_id;
    active_remote_ = active_remote;
    target_++;
    resolve_after_us_ = 0;
    pending_.clear();
    port_ = 0;
}

void Remote::clear() { set_target({}, {}, {}); }

void Remote::send(std::string command) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!dacp_id_.empty()) pending_.push_back(std::move(command));
}

bool Remote::send_now(const std::string &command) {
    sockaddr_storage peer;
    std::string active_remote;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!port_ || dacp_id_.empty()) return false;
        peer = peer_;
        active_remote = active_remote_;
    }
    return request(peer, port_, active_remote, command);
}

void Remote::loop() {
    while (!quit_) {
        sockaddr_storage peer;
        std::string dacp_id;
        std::string active_remote;
        std::vector<std::string> commands;
        uint32_t target;
        bool resolve = false;
        {
            std::lock_guard<std::mutex> guard(lock_);
            peer = peer_;
            dacp_id = dacp_id_;
            active_remote = active_remote_;
            target = target_;
            if (port_) {
                commands.swap(pending_);
            } else if (!dacp_id.empty() && esp_timer_get_time() >= resolve_after_us_) {
                resolve = true;
                resolve_after_us_ = esp_timer_get_time() + kResolveRetryUs;
            }
        }

        if (resolve) {
            const uint16_t port =
                resolve_port("iTunes_Ctrl_" + dacp_id, "_dacp._tcp", kResolveTimeoutMs);
            std::lock_guard<std::mutex> guard(lock_);
            if (target == target_ && port) {
                port_ = port;
                ESP_LOGI(TAG, "remote control on port %u", port);
            }
        }
        for (const std::string &command : commands) {
            if (request(peer, port_, active_remote, command)) continue;
            std::lock_guard<std::mutex> guard(lock_);
            if (target == target_) port_ = 0;
            break;
        }
        if (!resolve && commands.empty()) vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

bool Remote::request(const sockaddr_storage &peer, uint16_t port, const std::string &active_remote,
                     const std::string &command) {
    sockaddr_storage address = peer;
    socklen_t length;
    if (address.ss_family == AF_INET6) {
        reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port = htons(port);
        length = sizeof(sockaddr_in6);
    } else {
        reinterpret_cast<sockaddr_in *>(&address)->sin_port = htons(port);
        length = sizeof(sockaddr_in);
    }
    const int fd = socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    timeval timeout = { kTimeoutSeconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    char reply[16] = {};
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&address), length) == 0;
    if (ok) {
        const std::string text = "GET /ctrl-int/1/" + command +
                                 " HTTP/1.1\r\nHost: dacp\r\nActive-Remote: " + active_remote +
                                 "\r\nConnection: close\r\n\r\n";
        ok = ::send(fd, text.data(), text.size(), 0) == (ssize_t)text.size() &&
             recv(fd, reply, sizeof(reply) - 1, 0) > 0;
    }
    close(fd);
    if (!ok) {
        ESP_LOGW(TAG, "remote control request %s failed", command.c_str());
        return false;
    }
    if (strncmp(reply, "HTTP/1.", 7) != 0 || reply[9] != '2') {
        ESP_LOGW(TAG, "remote control %s: %s", command.c_str(), reply);
    }
    return true;
}

}  // namespace airplay
