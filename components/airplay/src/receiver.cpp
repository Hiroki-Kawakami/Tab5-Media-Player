/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "airplay.hpp"
#include "crypto.hpp"
#include "discovery.hpp"
#include "remote.hpp"
#include "stream.hpp"
#include "task.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/select.h>
#include <unistd.h>

namespace airplay {

static const char *TAG = "airplay";

static constexpr int kMaxClients = 4;
static constexpr std::size_t kRequestBytes = 16 * 1024;
static constexpr std::size_t kMaxBodyBytes = 2 * 1024 * 1024;
static constexpr int kPollMs = 100;
static constexpr uint32_t kStackBytes = 8192;

struct Request {
    std::string_view method;
    std::string_view uri;
    std::string_view headers;
    std::string_view body;
    std::shared_ptr<const uint8_t> storage;

    std::string_view header(std::string_view name) const {
        std::string_view rest = headers;
        while (!rest.empty()) {
            const std::size_t end = rest.find("\r\n");
            std::string_view line = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 2);
            const std::size_t colon = line.find(':');
            if (colon != name.size()) continue;
            bool match = true;
            for (std::size_t i = 0; i < colon && match; i++) {
                match = tolower((unsigned char)line[i]) == tolower((unsigned char)name[i]);
            }
            if (!match) continue;
            line.remove_prefix(colon + 1);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            return line;
        }
        return {};
    }
};

struct Client {
    int fd = -1;
    sockaddr_storage peer = {};
    sockaddr_storage local = {};
    char *buffer = nullptr;
    std::size_t used = 0;
    std::size_t discard = 0;
    std::string head;
    uint8_t *body = nullptr;
    std::size_t body_size = 0;
    std::size_t body_got = 0;
};

static void parse_head(std::string_view head, Request *request) {
    const std::size_t line_end = head.find("\r\n");
    const std::string_view line = head.substr(0, line_end);
    const std::size_t space = line.find(' ');
    request->method = line.substr(0, space);
    if (space != std::string_view::npos) {
        const std::string_view rest = line.substr(space + 1);
        request->uri = rest.substr(0, rest.find(' '));
    }
    request->headers =
        line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool parse_param(std::string_view text, std::string_view key, uint32_t *value) {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) return false;
    *value = (uint32_t)strtoul(std::string(text.substr(at + key.size(), 16)).c_str(), nullptr, 10);
    return true;
}

class Receiver {
public:
    Receiver(const Config &config, std::weak_ptr<Listener> listener)
        : config_(config), listener_(std::move(listener)) {}
    ~Receiver();
    bool start();
    State state() const { return state_; }
    NowPlaying now_playing();
    void command(std::string command) { remote_.send(std::move(command)); }

private:
    bool open_listeners();
    void serve_loop();
    void accept_client(int listener);
    void close_client(Client &client);
    void read_client(Client &client);
    std::size_t handle_buffer(Client &client);
    void handle(Client &client, const Request &request);
    void respond(Client &client, const Request &request, int code, const std::string &headers,
                 const std::string &body = {});
    std::string challenge_response(const Client &client, std::string_view challenge);
    bool announce(const Request &request);
    std::string setup(Client &client, const Request &request);
    void set_parameter(const Request &request);
    void parse_dmap(const uint8_t *data, std::size_t size);
    void set_artwork(std::shared_ptr<const uint8_t> data, std::size_t size);
    void end_session();
    void set_state(State state);

    Config config_;
    std::weak_ptr<Listener> listener_;
    int listeners_[2] = { -1, -1 };
    uint16_t port_ = 0;
    Client clients_[kMaxClients];
    int owner_ = -1;

    AirportKey key_;
    StreamSetup pending_;
    std::unique_ptr<Stream> stream_;
    std::unique_ptr<Advertiser> advertiser_;
    Remote remote_;
    std::mutex meta_lock_;
    std::string title_;
    std::string artist_;
    std::string album_;
    bool has_progress_ = false;
    uint32_t progress_start_ = 0;
    uint32_t progress_end_ = 0;
    uint32_t rate_ = 44100;
    bool has_volume_ = false;
    std::shared_ptr<const uint8_t> artwork_;
    std::size_t artwork_size_ = 0;
    float volume_db_ = 0;
    std::atomic<bool> has_position_{ false };
    std::atomic<uint32_t> position_rtp_{ 0 };
    std::atomic<State> state_{ State::Stopped };
    std::atomic<bool> quit_{ false };
    Task task_;
};

static Receiver *s_receiver;

Receiver::~Receiver() {
    quit_ = true;
    task_.join();
    remote_.stop();
    if (owner_ >= 0) remote_.send_now("pause");
    advertiser_.reset();
    end_session();
    for (Client &client : clients_) close_client(client);
    for (int fd : listeners_) {
        if (fd >= 0) close(fd);
    }
    set_state(State::Stopped);
}

bool Receiver::open_listeners() {
    const int v4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (v4 < 0) return false;
    listeners_[0] = v4;
    sockaddr_in address4 = {};
    address4.sin_family = AF_INET;
    address4.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t length = sizeof(address4);
    if (bind(v4, reinterpret_cast<sockaddr *>(&address4), length) != 0 || listen(v4, 2) != 0 ||
        getsockname(v4, reinterpret_cast<sockaddr *>(&address4), &length) != 0) {
        return false;
    }
    port_ = ntohs(address4.sin_port);

    const int v6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (v6 < 0) return true;
    int only = 1;
    setsockopt(v6, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only));
    sockaddr_in6 address6 = {};
    address6.sin6_family = AF_INET6;
    address6.sin6_addr = in6addr_any;
    address6.sin6_port = htons(port_);
    if (bind(v6, reinterpret_cast<sockaddr *>(&address6), sizeof(address6)) != 0 ||
        listen(v6, 2) != 0) {
        close(v6);
        return true;
    }
    listeners_[1] = v6;
    return true;
}

bool Receiver::start() {
    if (!open_listeners()) {
        ESP_LOGE(TAG, "cannot listen");
        return false;
    }

    char id[13];
    snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X", config_.mac[0], config_.mac[1],
             config_.mac[2], config_.mac[3], config_.mac[4], config_.mac[5]);
    std::string host;
    for (char c : config_.name) {
        if (isalnum((unsigned char)c)) host += (char)tolower((unsigned char)c);
    }
    host += "-" + std::string(id + 6);
    const TxtRecord txt = {
        { "txtvers", "1" }, { "ch", "2" },      { "cn", "0,1" },     { "da", "true" },
        { "et", "0,1" },    { "ek", "1" },      { "sr", "44100" },   { "ss", "16" },
        { "sv", "false" },  { "tp", "UDP" },    { "vn", "65537" },   { "vs", "105.1" },
        { "am", config_.model },   { "pw", "false" },  { "sf", "0x4" },
        { "md", "0,1,2" },
    };
    advertiser_ = Advertiser::create(host, std::string(id) + "@" + config_.name, "_raop._tcp",
                                     port_, txt);
    if (!advertiser_ || !remote_.start()) return false;

    set_state(State::Listening);
    ESP_LOGI(TAG, "listening on %u", port_);
    return task_.start("airplay_rtsp", kStackBytes, 4, [this] { serve_loop(); });
}

void Receiver::set_state(State state) {
    if (state_.exchange(state) == state) return;
    if (auto listener = listener_.lock()) listener->on_airplay_state(state);
}

void Receiver::serve_loop() {
    while (!quit_) {
        fd_set readable;
        FD_ZERO(&readable);
        int top = -1;
        auto watch = [&](int fd) {
            if (fd < 0) return;
            FD_SET(fd, &readable);
            if (fd > top) top = fd;
        };
        for (int fd : listeners_) watch(fd);
        for (const Client &client : clients_) watch(client.fd);
        timeval timeout = { 0, kPollMs * 1000 };
        if (select(top + 1, &readable, nullptr, nullptr, &timeout) <= 0) continue;

        for (int fd : listeners_) {
            if (fd >= 0 && FD_ISSET(fd, &readable)) accept_client(fd);
        }
        for (Client &client : clients_) {
            if (client.fd >= 0 && FD_ISSET(client.fd, &readable)) read_client(client);
        }
    }
}

void Receiver::accept_client(int listener) {
    Client *slot = nullptr;
    for (int i = 0; i < kMaxClients && !slot; i++) {
        if (clients_[i].fd < 0) slot = &clients_[i];
    }
    sockaddr_storage peer = {};
    socklen_t length = sizeof(peer);
    const int fd = accept(listener, reinterpret_cast<sockaddr *>(&peer), &length);
    if (fd < 0) return;
    if (!slot) {
        close(fd);
        return;
    }
    slot->buffer = static_cast<char *>(heap_caps_malloc(kRequestBytes, MALLOC_CAP_SPIRAM));
    if (!slot->buffer) {
        close(fd);
        return;
    }
    slot->fd = fd;
    slot->peer = peer;
    length = sizeof(slot->local);
    getsockname(fd, reinterpret_cast<sockaddr *>(&slot->local), &length);
    slot->used = 0;
    slot->discard = 0;
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

void Receiver::close_client(Client &client) {
    if (client.fd < 0) return;
    if (&client - clients_ == owner_) end_session();
    close(client.fd);
    heap_caps_free(client.buffer);
    heap_caps_free(client.body);
    client = Client();
}

void Receiver::end_session() {
    stream_.reset();
    owner_ = -1;
    remote_.clear();
    {
        std::lock_guard<std::mutex> guard(meta_lock_);
        title_.clear();
        artist_.clear();
        album_.clear();
        has_progress_ = false;
    }
    has_position_ = false;
    set_artwork(nullptr, 0);
    if (state_ != State::Stopped) set_state(State::Listening);
}

void Receiver::read_client(Client &client) {
    if (client.body) {
        const ssize_t got =
            recv(client.fd, client.body + client.body_got, client.body_size - client.body_got, 0);
        if (got <= 0) {
            close_client(client);
            return;
        }
        client.body_got += (std::size_t)got;
        if (client.body_got < client.body_size) return;
        const std::string head = std::move(client.head);
        Request request;
        parse_head(head, &request);
        request.storage = std::shared_ptr<const uint8_t>(client.body, heap_caps_free);
        request.body = std::string_view(reinterpret_cast<const char *>(client.body), client.body_size);
        client.body = nullptr;
        client.head.clear();
        handle(client, request);
        return;
    }
    if (client.discard) {
        char scrap[512];
        const ssize_t got =
            recv(client.fd, scrap, client.discard < sizeof(scrap) ? client.discard : sizeof(scrap), 0);
        if (got <= 0) {
            close_client(client);
            return;
        }
        client.discard -= (std::size_t)got;
        return;
    }
    const ssize_t got = recv(client.fd, client.buffer + client.used, kRequestBytes - client.used, 0);
    if (got <= 0) {
        close_client(client);
        return;
    }
    client.used += (std::size_t)got;
    while (client.fd >= 0 && client.used > 0) {
        const std::size_t consumed = handle_buffer(client);
        if (!consumed) break;
        memmove(client.buffer, client.buffer + consumed, client.used - consumed);
        client.used -= consumed;
    }
    if (client.fd >= 0 && client.used == kRequestBytes) {
        ESP_LOGW(TAG, "request too large");
        close_client(client);
    }
}

std::size_t Receiver::handle_buffer(Client &client) {
    const std::string_view data(client.buffer, client.used);
    const std::size_t end = data.find("\r\n\r\n");
    if (end == std::string_view::npos) return 0;
    Request request;
    parse_head(data.substr(0, end), &request);

    const std::string_view length_text = request.header("Content-Length");
    const std::size_t length =
        length_text.empty() ? 0 : (std::size_t)strtoul(std::string(length_text).c_str(), nullptr, 10);
    const std::size_t total = end + 4 + length;
    if (total > kRequestBytes && length <= kMaxBodyBytes) {
        client.body = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM));
    }
    if (client.body) {
        client.head = std::string(data.substr(0, end));
        client.body_size = length;
        client.body_got = client.used - (end + 4);
        memcpy(client.body, client.buffer + end + 4, client.body_got);
        return client.used;
    }
    if (total > kRequestBytes) {
        ESP_LOGW(TAG, "dropping a %u-byte body", (unsigned)length);
        request.body = {};
        handle(client, request);
        client.discard = total - client.used;
        return client.used;
    }
    if (client.used < total) return 0;
    request.body = data.substr(end + 4, length);
    handle(client, request);
    return total;
}

void Receiver::respond(Client &client, const Request &request, int code,
                       const std::string &headers, const std::string &body) {
    const char *reason = code == 200   ? "OK"
                         : code == 400 ? "Bad Request"
                         : code == 453 ? "Not Enough Bandwidth"
                                       : "Not Implemented";
    std::string response = "RTSP/1.0 " + std::to_string(code) + " " + reason + "\r\n";
    const std::string_view cseq = request.header("CSeq");
    if (!cseq.empty()) response += "CSeq: " + std::string(cseq) + "\r\n";
    response += "Server: AirTunes/105.1\r\n";
    const std::string_view challenge = request.header("Apple-Challenge");
    if (!challenge.empty()) {
        const std::string answer = challenge_response(client, challenge);
        if (!answer.empty()) response += "Apple-Response: " + answer + "\r\n";
    }
    response += headers;
    if (!body.empty()) response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "\r\n";
    response += body;
    send(client.fd, response.data(), response.size(), 0);
}

std::string Receiver::challenge_response(const Client &client, std::string_view challenge) {
    const std::vector<uint8_t> nonce = base64_decode(challenge);
    if (nonce.empty() || nonce.size() > 16) return {};
    uint8_t message[48] = {};
    std::size_t len = nonce.size();
    memcpy(message, nonce.data(), len);
    if (client.local.ss_family == AF_INET6) {
        memcpy(message + len, &reinterpret_cast<const sockaddr_in6 *>(&client.local)->sin6_addr, 16);
        len += 16;
    } else {
        memcpy(message + len, &reinterpret_cast<const sockaddr_in *>(&client.local)->sin_addr, 4);
        len += 4;
    }
    memcpy(message + len, config_.mac, sizeof(config_.mac));
    len += sizeof(config_.mac);
    if (len < 32) len = 32;

    std::vector<uint8_t> signature;
    if (!key_.sign_challenge(message, len, &signature)) {
        ESP_LOGE(TAG, "cannot sign the challenge");
        return {};
    }
    std::string answer = base64_encode(signature.data(), signature.size());
    while (!answer.empty() && answer.back() == '=') answer.pop_back();
    return answer;
}

bool Receiver::announce(const Request &request) {
    StreamSetup setup;
    bool have_codec = false;
    bool have_fmtp = false;
    std::vector<uint8_t> wrapped;
    std::vector<uint8_t> iv;
    std::string_view rest = request.body;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.starts_with("a=rtpmap:")) {
            const std::size_t space = line.find(' ');
            if (space == std::string_view::npos) continue;
            const std::string_view encoding = line.substr(space + 1);
            if (encoding.starts_with("AppleLossless")) {
                setup.format.codec = Codec::Alac;
                have_codec = true;
            } else if (encoding.starts_with("L16")) {
                setup.format.codec = Codec::Pcm;
                unsigned rate = 0, channels = 0;
                if (sscanf(std::string(encoding).c_str(), "L16/%u/%u", &rate, &channels) == 2) {
                    setup.format.rate = rate;
                    setup.format.channels = (uint8_t)channels;
                }
                have_codec = true;
            }
        } else if (line.starts_with("a=fmtp:")) {
            unsigned v[12];
            if (sscanf(std::string(line).c_str(), "a=fmtp:%u %u %u %u %u %u %u %u %u %u %u %u",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
                       &v[10], &v[11]) != 12) {
                continue;
            }
            Format &format = setup.format;
            format.frames_per_packet = v[1];
            format.channels = (uint8_t)v[7];
            format.rate = v[11];
            uint8_t *c = format.alac_cookie;
            c[0] = (uint8_t)(v[1] >> 24);
            c[1] = (uint8_t)(v[1] >> 16);
            c[2] = (uint8_t)(v[1] >> 8);
            c[3] = (uint8_t)v[1];
            c[4] = (uint8_t)v[2];
            c[5] = (uint8_t)v[3];
            c[6] = (uint8_t)v[4];
            c[7] = (uint8_t)v[5];
            c[8] = (uint8_t)v[6];
            c[9] = (uint8_t)v[7];
            c[10] = (uint8_t)(v[8] >> 8);
            c[11] = (uint8_t)v[8];
            for (int i = 0; i < 3; i++) {
                const uint32_t value = v[9 + i];
                c[12 + 4 * i] = (uint8_t)(value >> 24);
                c[13 + 4 * i] = (uint8_t)(value >> 16);
                c[14 + 4 * i] = (uint8_t)(value >> 8);
                c[15 + 4 * i] = (uint8_t)value;
            }
            have_fmtp = v[3] == 16;
        } else if (line.starts_with("a=rsaaeskey:")) {
            wrapped = base64_decode(line.substr(12));
        } else if (line.starts_with("a=aesiv:")) {
            iv = base64_decode(line.substr(8));
        }
    }
    if (!have_codec || (setup.format.codec == Codec::Alac && !have_fmtp)) {
        ESP_LOGW(TAG, "unsupported ANNOUNCE");
        return false;
    }
    if (!wrapped.empty()) {
        std::vector<uint8_t> key;
        if (!key_.decrypt_key(wrapped, &key) || key.size() != 16 || iv.size() != 16) {
            ESP_LOGE(TAG, "cannot unwrap the session key");
            return false;
        }
        setup.encrypted = true;
        memcpy(setup.key, key.data(), 16);
        memcpy(setup.iv, iv.data(), 16);
    }
    pending_ = setup;
    return true;
}

std::string Receiver::setup(Client &client, const Request &request) {
    const std::string_view transport = request.header("Transport");
    uint32_t control = 0;
    uint32_t timing = 0;
    parse_param(transport, "control_port=", &control);
    parse_param(transport, "timing_port=", &timing);
    StreamSetup setup = pending_;
    setup.output = config_.output.get();
    setup.on_position = [this](uint32_t rtp) {
        position_rtp_ = rtp;
        has_position_ = true;
    };
    {
        std::lock_guard<std::mutex> guard(meta_lock_);
        rate_ = setup.format.rate;
    }
    setup.peer = client.peer;
    setup.peer_control_port = (uint16_t)control;
    setup.peer_timing_port = (uint16_t)timing;

    stream_.reset();
    stream_ = Stream::create(setup, [this](bool playing) {
        set_state(playing ? State::Playing : State::Connected);
    });
    if (!stream_) return {};
    char text[160];
    snprintf(text, sizeof(text),
             "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;control_port=%u;"
             "timing_port=%u\r\n",
             stream_->data_port(), stream_->control_port(), stream_->timing_port());
    return std::string(text) + "Session: 1\r\nAudio-Jack-Status: connected; type=analog\r\n";
}

void Receiver::set_parameter(const Request &request) {
    const std::string_view type = request.header("Content-Type");
    const auto *bytes = reinterpret_cast<const uint8_t *>(request.body.data());
    if (type == "application/x-dmap-tagged") {
        parse_dmap(bytes, request.body.size());
        return;
    }
    if (type.starts_with("image/")) {
        if (type == "image/none" || request.body.empty()) {
            set_artwork(nullptr, 0);
            return;
        }
        std::shared_ptr<const uint8_t> data = request.storage;
        if (!data) {
            auto *copy = static_cast<uint8_t *>(heap_caps_malloc(request.body.size(), MALLOC_CAP_SPIRAM));
            if (!copy) return;
            memcpy(copy, bytes, request.body.size());
            data = std::shared_ptr<const uint8_t>(copy, heap_caps_free);
        }
        set_artwork(std::move(data), request.body.size());
        return;
    }
    if (type != "text/parameters") return;

    const std::string text(request.body);
    if (const char *volume = strstr(text.c_str(), "volume:")) {
        const float db = strtof(volume + 7, nullptr);
        {
            std::lock_guard<std::mutex> guard(meta_lock_);
            has_volume_ = true;
            volume_db_ = db;
        }
        if (auto listener = listener_.lock()) listener->on_airplay_volume(db);
    }
    if (const char *progress = strstr(text.c_str(), "progress:")) {
        unsigned long start = 0, current = 0, end = 0;
        if (sscanf(progress + 9, " %lu/%lu/%lu", &start, &current, &end) == 3) {
            std::lock_guard<std::mutex> guard(meta_lock_);
            has_progress_ = true;
            progress_start_ = (uint32_t)start;
            progress_end_ = (uint32_t)end;
        }
    }
}

void Receiver::parse_dmap(const uint8_t *data, std::size_t size) {
    while (size >= 8) {
        const uint32_t length = be32(data + 4);
        if (length > size - 8) return;
        const std::string_view tag(reinterpret_cast<const char *>(data), 4);
        const std::string value(reinterpret_cast<const char *>(data + 8), length);
        if (tag == "mlit") {
            {
                std::lock_guard<std::mutex> guard(meta_lock_);
                title_.clear();
                artist_.clear();
                album_.clear();
            }
            parse_dmap(data + 8, length);
        } else if (tag == "minm" || tag == "asar" || tag == "asal") {
            std::lock_guard<std::mutex> guard(meta_lock_);
            (tag == "minm" ? title_ : tag == "asar" ? artist_ : album_) = value;
        }
        data += 8 + length;
        size -= 8 + length;
    }
}

void Receiver::set_artwork(std::shared_ptr<const uint8_t> data, std::size_t size) {
    if (!data && !artwork_) return;
    if (data && artwork_ && size == artwork_size_ && memcmp(data.get(), artwork_.get(), size) == 0) {
        return;
    }
    artwork_ = data;
    artwork_size_ = size;
    if (auto listener = listener_.lock()) listener->on_airplay_artwork(std::move(data), size);
}

NowPlaying Receiver::now_playing() {
    NowPlaying now;
    now.state = state_;
    now.remote = remote_.available();
    std::lock_guard<std::mutex> guard(meta_lock_);
    now.title = title_;
    now.artist = artist_;
    now.album = album_;
    now.has_volume = has_volume_;
    now.volume_db = volume_db_;
    if (has_progress_ && rate_) {
        now.duration_ms = (int64_t)(uint32_t)(progress_end_ - progress_start_) * 1000 / rate_;
        if (has_position_) {
            const int32_t played = (int32_t)(position_rtp_.load() - progress_start_);
            now.position_ms = played < 0 ? 0 : (int64_t)played * 1000 / rate_;
            if (now.position_ms > now.duration_ms) now.position_ms = now.duration_ms;
        }
    }
    return now;
}

void Receiver::handle(Client &client, const Request &request) {
    const int index = (int)(&client - clients_);
    const std::string_view method = request.method;
    ESP_LOGD(TAG, "%.*s %.*s", (int)method.size(), method.data(), (int)request.uri.size(),
             request.uri.data());
    const std::string_view dacp_id = request.header("DACP-ID");
    const std::string_view active_remote = request.header("Active-Remote");

    if (method == "OPTIONS") {
        respond(client, request, 200,
                "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
                "GET_PARAMETER, SET_PARAMETER\r\n");
    } else if (method == "ANNOUNCE") {
        if (!announce(request)) {
            respond(client, request, 400, {});
            return;
        }
        if (owner_ != index) {
            for (Client &other : clients_) {
                if (&other != &client && &other - clients_ == owner_) close_client(other);
            }
        }
        end_session();
        owner_ = index;
        set_state(State::Connected);
        if (!dacp_id.empty() && !active_remote.empty()) {
            remote_.set_target(client.peer, std::string(dacp_id), std::string(active_remote));
        }
        respond(client, request, 200, {});
    } else if (index != owner_) {
        respond(client, request, method == "POST" || method == "GET_PARAMETER" ? 200 : 453, {});
    } else if (method == "SETUP") {
        const std::string transport = setup(client, request);
        respond(client, request, transport.empty() ? 400 : 200, transport);
    } else if (method == "RECORD") {
        if (stream_) stream_->record();
        respond(client, request, 200, "Audio-Latency: 11025\r\n");
    } else if (method == "FLUSH") {
        uint32_t rtp = 0;
        const bool has_rtp = parse_param(request.header("RTP-Info"), "rtptime=", &rtp);
        if (stream_) stream_->flush(has_rtp, rtp);
        respond(client, request, 200, {});
    } else if (method == "SET_PARAMETER") {
        set_parameter(request);
        respond(client, request, 200, {});
    } else if (method == "TEARDOWN") {
        stream_.reset();
        set_state(State::Connected);
        respond(client, request, 200, "Connection: close\r\n");
    } else if (method == "GET_PARAMETER" || method == "POST") {
        respond(client, request, 200, {});
    } else {
        respond(client, request, 501, {});
    }
}

bool start(const Config &config, std::weak_ptr<Listener> listener) {
    stop();
    auto *receiver = new Receiver(config, std::move(listener));
    if (!receiver->start()) {
        delete receiver;
        return false;
    }
    s_receiver = receiver;
    return true;
}

void stop() {
    delete s_receiver;
    s_receiver = nullptr;
}

State state() { return s_receiver ? s_receiver->state() : State::Stopped; }

NowPlaying now_playing() {
    if (!s_receiver) return {};
    return s_receiver->now_playing();
}

void remote(Command command) {
    if (!s_receiver) return;
    switch (command) {
    case Command::PlayPause: s_receiver->command("playpause"); break;
    case Command::Next: s_receiver->command("nextitem"); break;
    case Command::Previous: s_receiver->command("previtem"); break;
    }
}

}  // namespace airplay
