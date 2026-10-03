/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/socket.h>
#include "airplay.hpp"
#include "crypto.hpp"
#include "decoder.hpp"
#include "task.hpp"

namespace airplay {

struct StreamSetup {
    Output *output = nullptr;
    Format format;
    bool encrypted = false;
    uint8_t key[16] = {};
    uint8_t iv[16] = {};
    sockaddr_storage peer = {};
    uint16_t peer_control_port = 0;
    uint16_t peer_timing_port = 0;
};

class Stream {
public:
    using PlayingFn = std::function<void(bool playing)>;

    static std::unique_ptr<Stream> create(const StreamSetup &setup, PlayingFn on_playing);
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
    ~Stream();

    uint16_t data_port() const { return ports_[kData]; }
    uint16_t control_port() const { return ports_[kControl]; }
    uint16_t timing_port() const { return ports_[kTiming]; }

    void record();
    void flush(bool has_rtp, uint32_t rtp);

private:
    enum { kData, kControl, kTiming, kSocketCount };

    struct Slot {
        uint32_t rtp;
        uint16_t seq;
        uint16_t frames;
        bool full;
    };

    struct ClockSample {
        int64_t rtt_ns;
        int64_t offset_ns;
    };

    Stream(const StreamSetup &setup, PlayingFn on_playing);
    bool init();
    bool open_socket(int index);

    void receive_loop();
    void handle_audio(const uint8_t *packet, std::size_t len, bool resent);
    void handle_sync(const uint8_t *packet, std::size_t len);
    void handle_timing(const uint8_t *packet, std::size_t len);
    void send_timing_request();
    void request_resend(uint16_t first, uint16_t count);
    void send_to(int index, uint16_t port, const uint8_t *data, std::size_t len);

    void play_loop();
    bool start_playing();
    bool play_packet();
    void stop_playing();
    bool find_head_locked();
    int64_t play_time_ns_locked(uint32_t rtp) const;
    int16_t *slot_pcm(uint16_t seq) const;
    void write_output(int16_t *pcm, uint32_t frames);

    StreamSetup setup_;
    PlayingFn on_playing_;
    uint32_t frame_bytes_;
    uint32_t packet_ns_;
    int sockets_[kSocketCount] = { -1, -1, -1 };
    uint16_t ports_[kSocketCount] = {};

    AesCbc aes_;
    Decoder decoder_;
    uint8_t *packet_ = nullptr;
    uint8_t *plain_ = nullptr;
    int16_t *decoded_ = nullptr;
    std::atomic<bool> have_expected_{ false };
    uint16_t expected_seq_ = 0;
    int64_t next_timing_ns_ = 0;
    int64_t timing_sent_ns_ = 0;
    uint32_t timing_count_ = 0;
    ClockSample clock_samples_[8] = {};

    std::mutex lock_;
    Slot *slots_ = nullptr;
    int16_t *pcm_ = nullptr;
    bool recording_ = false;
    bool playing_ = false;
    bool have_head_ = false;
    uint16_t head_seq_ = 0;
    bool have_latest_ = false;
    uint16_t latest_seq_ = 0;
    uint16_t play_seq_ = 0;
    bool flush_pending_ = false;
    uint32_t flush_rtp_ = 0;
    bool clock_valid_ = false;
    int64_t clock_offset_ns_ = 0;
    bool anchor_valid_ = false;
    uint32_t anchor_rtp_ = 0;
    int64_t anchor_remote_ns_ = 0;

    int16_t *out_ = nullptr;
    bool output_open_ = false;
    uint32_t play_rtp_ = 0;
    uint32_t starved_ = 0;
    uint32_t played_ = 0;
    int64_t error_ns_ = 0;
    int64_t baseline_ns_ = 0;

    std::atomic<bool> quit_{ false };
    Task receiver_;
    Task player_;
};

}  // namespace airplay
