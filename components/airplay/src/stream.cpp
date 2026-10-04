/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "stream.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <unistd.h>

namespace airplay {

static const char *TAG = "airplay";

static constexpr uint32_t kSlots = 512;
static constexpr uint32_t kMaxFramesPerPacket = 4096;
static constexpr std::size_t kPacketBytes = 2048;
static constexpr uint32_t kLatencyOffsetFrames = 11025;
static constexpr int64_t kMs = 1000000;
static constexpr int64_t kPollMs = 20;
static constexpr int64_t kTimingFastNs = 300 * kMs;
static constexpr int64_t kTimingNs = 3000 * kMs;
static constexpr uint32_t kTimingFastCount = 4;
static constexpr int64_t kMaxRttNs = 200 * kMs;
static constexpr int64_t kPrefillNs = 100 * kMs;
static constexpr int64_t kStarveNs = 1000 * kMs;
static constexpr int64_t kDriftNs = 2 * kMs;
static constexpr int64_t kResyncNs = 50 * kMs;
static constexpr uint32_t kWarmupPackets = 32;
static constexpr uint32_t kSettlePackets = 256;
static constexpr int kMaxResend = 128;

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static int64_t ntp_to_ns(const uint8_t *p) {
    return (int64_t)be32(p) * 1000000000 + (int64_t)(((uint64_t)be32(p + 4) * 1000000000) >> 32);
}

static int64_t now_ns() { return esp_timer_get_time() * 1000; }

static bool seq_before(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

std::unique_ptr<Stream> Stream::create(const StreamSetup &setup, PlayingFn on_playing) {
    std::unique_ptr<Stream> stream(new Stream(setup, std::move(on_playing)));
    if (!stream->init()) return nullptr;
    return stream;
}

Stream::Stream(const StreamSetup &setup, PlayingFn on_playing)
    : setup_(setup), on_playing_(std::move(on_playing)),
      frame_bytes_(setup.format.channels * sizeof(int16_t)),
      packet_ns_((uint32_t)((int64_t)setup.format.frames_per_packet * 1000000000 /
                            setup.format.rate)) {}

bool Stream::init() {
    const Format &format = setup_.format;
    if (!format.rate || !format.channels || format.channels > 2 || !format.frames_per_packet ||
        format.frames_per_packet > kMaxFramesPerPacket) {
        return false;
    }
    if (setup_.encrypted && !aes_.init(setup_.key, setup_.iv)) {
        ESP_LOGE(TAG, "AES key rejected");
        return false;
    }
    if (!decoder_.open(format)) {
        ESP_LOGE(TAG, "decoder unavailable");
        return false;
    }

    const std::size_t packet_frames = format.frames_per_packet + 1;
    packet_ = static_cast<uint8_t *>(heap_caps_malloc(kPacketBytes * 2, MALLOC_CAP_SPIRAM));
    decoded_ = static_cast<int16_t *>(
        heap_caps_malloc(packet_frames * frame_bytes_, MALLOC_CAP_SPIRAM));
    out_ = static_cast<int16_t *>(heap_caps_malloc(packet_frames * frame_bytes_, MALLOC_CAP_SPIRAM));
    slots_ = static_cast<Slot *>(heap_caps_calloc(kSlots, sizeof(Slot), MALLOC_CAP_SPIRAM));
    pcm_ = static_cast<int16_t *>(
        heap_caps_malloc((std::size_t)kSlots * format.frames_per_packet * frame_bytes_,
                         MALLOC_CAP_SPIRAM));
    if (!packet_ || !decoded_ || !out_ || !slots_ || !pcm_) {
        ESP_LOGE(TAG, "out of memory for the stream buffers");
        return false;
    }
    plain_ = packet_ + kPacketBytes;

    for (int i = 0; i < kSocketCount; i++) {
        if (!open_socket(i)) return false;
    }
    return receiver_.start("airplay_rx", 6144, 5, [this] { receive_loop(); }) &&
           player_.start("airplay_play", 4096, 6, [this] { play_loop(); });
}

Stream::~Stream() {
    quit_ = true;
    receiver_.join();
    player_.join();
    for (int socket : sockets_) {
        if (socket >= 0) close(socket);
    }
    heap_caps_free(packet_);
    heap_caps_free(decoded_);
    heap_caps_free(out_);
    heap_caps_free(slots_);
    heap_caps_free(pcm_);
}

bool Stream::open_socket(int index) {
    const int family = setup_.peer.ss_family;
    const int fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    sockets_[index] = fd;

    sockaddr_storage address = {};
    socklen_t length;
    if (family == AF_INET6) {
        auto *in6 = reinterpret_cast<sockaddr_in6 *>(&address);
        in6->sin6_family = AF_INET6;
        in6->sin6_addr = in6addr_any;
        length = sizeof(sockaddr_in6);
    } else {
        auto *in4 = reinterpret_cast<sockaddr_in *>(&address);
        in4->sin_family = AF_INET;
        in4->sin_addr.s_addr = htonl(INADDR_ANY);
        length = sizeof(sockaddr_in);
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), length) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return false;
    }
    ports_[index] = family == AF_INET6
                        ? ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port)
                        : ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
    return true;
}

void Stream::record() {
    std::lock_guard<std::mutex> guard(lock_);
    recording_ = true;
}

void Stream::flush(bool has_rtp, uint32_t rtp) {
    std::lock_guard<std::mutex> guard(lock_);
    for (uint32_t i = 0; i < kSlots; i++) slots_[i].full = false;
    have_head_ = false;
    have_latest_ = false;
    have_expected_ = false;
    playing_ = false;
    flush_pending_ = has_rtp;
    flush_rtp_ = rtp;
    silent_after_flush_ = true;
}

void Stream::send_to(int index, uint16_t port, const uint8_t *data, std::size_t len) {
    sockaddr_storage peer = setup_.peer;
    socklen_t length;
    if (peer.ss_family == AF_INET6) {
        reinterpret_cast<sockaddr_in6 *>(&peer)->sin6_port = htons(port);
        length = sizeof(sockaddr_in6);
    } else {
        reinterpret_cast<sockaddr_in *>(&peer)->sin_port = htons(port);
        length = sizeof(sockaddr_in);
    }
    sendto(sockets_[index], data, len, 0, reinterpret_cast<sockaddr *>(&peer), length);
}

void Stream::receive_loop() {
    while (!quit_) {
        const int64_t now = now_ns();
        if (now >= next_timing_ns_) {
            send_timing_request();
            next_timing_ns_ = now + (timing_count_ < kTimingFastCount ? kTimingFastNs : kTimingNs);
        }

        fd_set readable;
        FD_ZERO(&readable);
        int top = -1;
        for (int socket : sockets_) {
            FD_SET(socket, &readable);
            if (socket > top) top = socket;
        }
        timeval timeout = { 0, (int)(kPollMs * 1000) };
        if (select(top + 1, &readable, nullptr, nullptr, &timeout) <= 0) continue;

        for (int i = 0; i < kSocketCount; i++) {
            if (!FD_ISSET(sockets_[i], &readable)) continue;
            const ssize_t got = recv(sockets_[i], packet_, kPacketBytes, 0);
            if (got < 4) continue;
            const std::size_t len = (std::size_t)got;
            const uint8_t type = packet_[1] & 0x7f;
            if (i == kData && type == 0x60) {
                handle_audio(packet_, len, false);
            } else if (i == kControl && type == 0x54) {
                handle_sync(packet_, len);
            } else if (i == kControl && type == 0x56) {
                handle_audio(packet_ + 4, len - 4, true);
            } else if (i == kTiming && type == 0x53) {
                handle_timing(packet_, len);
            }
        }
    }
}

void Stream::handle_audio(const uint8_t *packet, std::size_t len, bool resent) {
    if (len <= 12) return;
    const uint16_t seq = be16(packet + 2);
    const uint32_t rtp = be32(packet + 4);
    const uint8_t *payload = packet + 12;
    const std::size_t size = len - 12;

    if (!resent) {
        const int16_t gap = (int16_t)(seq - expected_seq_);
        if (have_expected_ && gap > 0 && gap < kMaxResend) request_resend(expected_seq_, gap);
        if (!have_expected_ || gap >= 0) expected_seq_ = (uint16_t)(seq + 1);
        have_expected_ = true;
    }

    if (setup_.encrypted) {
        const std::size_t blocks = size & ~(std::size_t)15;
        if (blocks && !aes_.decrypt(payload, plain_, blocks)) return;
        memcpy(plain_ + blocks, payload + blocks, size - blocks);
        payload = plain_;
    }
    const uint32_t frames =
        decoder_.decode(payload, size, decoded_, setup_.format.frames_per_packet);
    if (!frames) return;
    bool silent = true;
    for (uint32_t i = 0; i < frames * setup_.format.channels && silent; i++) silent = !decoded_[i];

    std::lock_guard<std::mutex> guard(lock_);
    if (flush_pending_) {
        if ((int32_t)(rtp - flush_rtp_) < 0) return;
        flush_pending_ = false;
    }
    if (silent_after_flush_) {
        if (silent) return;
        silent_after_flush_ = false;
    }
    if (playing_ && seq_before(seq, play_seq_)) return;
    Slot &slot = slots_[seq % kSlots];
    slot = { rtp, seq, (uint16_t)frames, true };
    memcpy(slot_pcm(seq), decoded_, frames * frame_bytes_);
    if (!have_head_ || seq_before(seq, head_seq_)) head_seq_ = seq;
    have_head_ = true;
    if (!have_latest_ || seq_before(latest_seq_, seq)) latest_seq_ = seq;
    have_latest_ = true;
}

void Stream::handle_sync(const uint8_t *packet, std::size_t len) {
    if (len < 20) return;
    std::lock_guard<std::mutex> guard(lock_);
    anchor_rtp_ = be32(packet + 4) - kLatencyOffsetFrames;
    anchor_remote_ns_ = ntp_to_ns(packet + 8);
    anchor_valid_ = true;
}

void Stream::send_timing_request() {
    uint8_t packet[32] = { 0x80, 0xd2, 0x00, 0x07 };
    timing_sent_ns_ = now_ns();
    put32(packet + 24, (uint32_t)(timing_sent_ns_ / 1000000000));
    put32(packet + 28, (uint32_t)(((uint64_t)(timing_sent_ns_ % 1000000000) << 32) / 1000000000));
    send_to(kTiming, setup_.peer_timing_port, packet, sizeof(packet));
}

void Stream::handle_timing(const uint8_t *packet, std::size_t len) {
    if (len < 32) return;
    const int64_t arrived = now_ns();
    const int64_t received = ntp_to_ns(packet + 16);
    const int64_t sent = ntp_to_ns(packet + 24);
    int64_t rtt = arrived - timing_sent_ns_;
    if (sent > received && sent - received < rtt) rtt -= sent - received;
    if (rtt <= 0 || rtt > kMaxRttNs) return;

    const ClockSample sample = { rtt, (received / 2 + sent / 2) - (timing_sent_ns_ / 2 + arrived / 2) };
    const std::size_t count = sizeof(clock_samples_) / sizeof(clock_samples_[0]);
    clock_samples_[timing_count_++ % count] = sample;
    const ClockSample *best = &clock_samples_[0];
    for (std::size_t i = 1; i < count && i < timing_count_; i++) {
        if (clock_samples_[i].rtt_ns < best->rtt_ns) best = &clock_samples_[i];
    }
    std::lock_guard<std::mutex> guard(lock_);
    clock_offset_ns_ = best->offset_ns;
    clock_valid_ = true;
}

void Stream::request_resend(uint16_t first, uint16_t count) {
    uint8_t packet[8] = { 0x80, 0xd5, 0x00, 0x01 };
    put16(packet + 4, first);
    put16(packet + 6, count);
    send_to(kControl, setup_.peer_control_port, packet, sizeof(packet));
}

int16_t *Stream::slot_pcm(uint16_t seq) const {
    return pcm_ + (std::size_t)(seq % kSlots) * setup_.format.frames_per_packet *
                      setup_.format.channels;
}

int64_t Stream::play_time_ns_locked(uint32_t rtp) const {
    const int64_t frames = (int32_t)(rtp - anchor_rtp_);
    return anchor_remote_ns_ + frames * 1000000000 / setup_.format.rate - clock_offset_ns_;
}

bool Stream::find_head_locked() {
    for (uint32_t i = 0; have_head_ && i < kSlots; i++) {
        const uint16_t seq = (uint16_t)(head_seq_ + i);
        const Slot &slot = slots_[seq % kSlots];
        if (slot.full && slot.seq == seq) {
            head_seq_ = seq;
            return true;
        }
        if (seq == latest_seq_) break;
    }
    have_head_ = false;
    return false;
}

void Stream::write_output(int16_t *pcm, uint32_t frames) {
    setup_.output->write(pcm, frames);
}

void Stream::play_loop() {
    while (!quit_) {
        bool ready = false;
        {
            std::lock_guard<std::mutex> guard(lock_);
            ready = playing_;
        }
        if (ready ? !play_packet() : !start_playing()) vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
    stop_playing();
}

bool Stream::start_playing() {
    bool due = false;
    int64_t wait = 0;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (recording_ && clock_valid_ && anchor_valid_ && find_head_locked()) {
            Slot &slot = slots_[head_seq_ % kSlots];
            wait = play_time_ns_locked(slot.rtp) - now_ns();
            if (wait + packet_ns_ < 0) {
                slot.full = false;
                head_seq_++;
                return true;
            }
            due = wait <= kPrefillNs;
            if (due) {
                playing_ = true;
                play_seq_ = head_seq_;
                play_rtp_ = slot.rtp;
            }
        }
    }
    if (!due) {
        if (output_open_) stop_playing();
        return false;
    }

    if (!setup_.output->open(setup_.format.rate, setup_.format.channels)) {
        ESP_LOGE(TAG, "cannot open the output");
        std::lock_guard<std::mutex> guard(lock_);
        playing_ = false;
        return false;
    }
    output_open_ = true;
    starved_ = 0;
    played_ = 0;
    if (on_playing_) on_playing_(true);

    uint32_t silence = wait > 0 ? (uint32_t)(wait * setup_.format.rate / 1000000000) : 0;
    while (silence > 0 && !quit_) {
        const uint32_t frames = silence < setup_.format.frames_per_packet
                                    ? silence
                                    : setup_.format.frames_per_packet;
        memset(out_, 0, frames * frame_bytes_);
        write_output(out_, frames);
        silence -= frames;
    }
    return true;
}

bool Stream::play_packet() {
    const uint32_t channels = setup_.format.channels;
    uint32_t frames;
    int64_t due;
    bool starving;
    {
        std::lock_guard<std::mutex> guard(lock_);
        Slot &slot = slots_[play_seq_ % kSlots];
        if (slot.full && slot.seq == play_seq_) {
            frames = slot.frames;
            play_rtp_ = slot.rtp;
            memcpy(out_, slot_pcm(play_seq_), frames * frame_bytes_);
            slot.full = false;
        } else {
            frames = setup_.format.frames_per_packet;
            memset(out_, 0, frames * frame_bytes_);
        }
        starving = !have_latest_ || seq_before(latest_seq_, play_seq_);
        due = play_time_ns_locked(play_rtp_);
        play_seq_++;
    }
    const uint32_t rtp = play_rtp_;
    play_rtp_ += frames;

    starved_ = starving ? starved_ + 1 : 0;
    if ((int64_t)starved_ * packet_ns_ > kStarveNs) {
        stop_playing();
        return false;
    }

    const int64_t error = due - now_ns();
    error_ns_ = played_ > kWarmupPackets ? error_ns_ + (error - error_ns_) / 64 : error;
    if (++played_ == kSettlePackets) baseline_ns_ = error_ns_;
    if (played_ > kSettlePackets) {
        const int64_t drift = error_ns_ - baseline_ns_;
        if (drift > kResyncNs || drift < -kResyncNs) {
            ESP_LOGW(TAG, "lost sync by %lld ms", (long long)(drift / kMs));
            stop_playing();
            return false;
        }
        if (drift > kDriftNs) {
            memcpy(out_ + frames * channels, out_ + (frames - 1) * channels, frame_bytes_);
            frames++;
        } else if (drift < -kDriftNs && frames > 1) {
            frames--;
        }
    }
    write_output(out_, frames);
    if (setup_.on_position) setup_.on_position(rtp);
    return true;
}

void Stream::stop_playing() {
    bool was_playing;
    {
        std::lock_guard<std::mutex> guard(lock_);
        was_playing = playing_ || output_open_;
        playing_ = false;
    }
    if (output_open_) setup_.output->close();
    output_open_ = false;
    if (was_playing && on_playing_) on_playing_(false);
}

}  // namespace airplay
