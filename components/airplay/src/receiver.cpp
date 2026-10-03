/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "airplay.hpp"
#include "advertiser.hpp"
#include "crypto.hpp"
#include "stream.hpp"
#include "task.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/select.h>
#include <unistd.h>

namespace airplay {

static const char *TAG = "airplay";

static constexpr int kMaxClients = 4;
static constexpr std::size_t kRequestBytes = 16 * 1024;
static constexpr int kPollMs = 100;
static constexpr uint32_t kStackBytes = 8192;

struct Request {
    std::string_view method;
    std::string_view uri;
    std::string_view headers;
    std::string_view body;

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
};

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
    std::atomic<State> state_{ State::Stopped };
    std::atomic<bool> quit_{ false };
    Task task_;
};

static Receiver *s_receiver;

Receiver::~Receiver() {
    quit_ = true;
    task_.join();
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
    };
    advertiser_ = Advertiser::create(host, std::string(id) + "@" + config_.name, "_raop._tcp",
                                     port_, txt);
    if (!advertiser_) return false;

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
    client = Client();
}

void Receiver::end_session() {
    stream_.reset();
    owner_ = -1;
    if (state_ != State::Stopped) set_state(State::Listening);
}

void Receiver::read_client(Client &client) {
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
    const std::string_view head = data.substr(0, end);
    const std::size_t line_end = head.find("\r\n");
    const std::string_view line = head.substr(0, line_end);

    Request request;
    const std::size_t space = line.find(' ');
    request.method = line.substr(0, space);
    if (space != std::string_view::npos) {
        const std::string_view rest = line.substr(space + 1);
        request.uri = rest.substr(0, rest.find(' '));
    }
    request.headers =
        line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);

    const std::string_view length_text = request.header("Content-Length");
    const std::size_t length =
        length_text.empty() ? 0 : (std::size_t)strtoul(std::string(length_text).c_str(), nullptr, 10);
    const std::size_t total = end + 4 + length;
    if (total > kRequestBytes) {
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
    if (request.header("Content-Type") != "text/parameters") return;
    const std::size_t at = request.body.find("volume:");
    if (at == std::string_view::npos) return;
    const float db = strtof(std::string(request.body.substr(at + 7, 32)).c_str(), nullptr);
    if (auto listener = listener_.lock()) listener->on_airplay_volume(db);
}

void Receiver::handle(Client &client, const Request &request) {
    const int index = (int)(&client - clients_);
    const std::string_view method = request.method;
    ESP_LOGD(TAG, "%.*s %.*s", (int)method.size(), method.data(), (int)request.uri.size(),
             request.uri.data());

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
        stream_.reset();
        owner_ = index;
        set_state(State::Connected);
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

}  // namespace airplay
