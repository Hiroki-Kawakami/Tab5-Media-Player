/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <vector>
#include "task.hpp"

namespace airplay {

/* DACP client: finds the sender's remote-control port and sends it commands from its own task. */
class Remote {
public:
    Remote() = default;
    Remote(const Remote &) = delete;
    Remote &operator=(const Remote &) = delete;
    ~Remote();

    bool start();
    void stop();
    void set_target(const sockaddr_storage &peer, const std::string &dacp_id,
                    const std::string &active_remote);
    void clear();
    bool available() const { return port_ != 0; }
    void send(std::string command);

private:
    void loop();
    bool request(const sockaddr_storage &peer, uint16_t port, const std::string &active_remote,
                 const std::string &command);

    std::mutex lock_;
    sockaddr_storage peer_ = {};
    std::string dacp_id_;
    std::string active_remote_;
    uint32_t target_ = 0;
    int64_t resolve_after_us_ = 0;
    std::vector<std::string> pending_;
    std::atomic<uint16_t> port_{ 0 };
    std::atomic<bool> quit_{ false };
    Task task_;
};

}  // namespace airplay
