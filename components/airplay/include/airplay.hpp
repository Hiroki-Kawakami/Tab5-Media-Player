/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace airplay {

enum class State {
    Stopped,
    Listening,
    Connected,
    Playing,
};

inline constexpr float kVolumeMute = -144.0f;
inline constexpr float kVolumeMin = -30.0f;

/* Called from the receiver's tasks, not the caller's. */
class Listener {
public:
    virtual ~Listener() = default;
    virtual void on_airplay_state(State state) = 0;
    /* kVolumeMute, or kVolumeMin..0 dB. */
    virtual void on_airplay_volume(float db) = 0;
    /* JPEG or PNG bytes; null when the track has none or the session ended. */
    virtual void on_airplay_artwork(std::shared_ptr<const uint8_t> data, std::size_t size) = 0;
};

/* Called from the receiver's playback task. */
class Output {
public:
    virtual ~Output() = default;
    virtual bool open(uint32_t rate, uint8_t channels) = 0;
    /* Has to block while the output is full: playback is paced and kept in sync by it. pcm may be
       modified in place. */
    virtual void write(int16_t *pcm, std::size_t frames) = 0;
    virtual void close() = 0;
};

struct Config {
    std::string name;
    std::string model;
    uint8_t mac[6];
    std::shared_ptr<Output> output;
};

/* Listens and advertises until stop(); one sender at a time, a new one takes over. */
bool start(const Config &config, std::weak_ptr<Listener> listener);
void stop();
State state();

struct NowPlaying {
    State state = State::Stopped;
    std::string title;
    std::string artist;
    std::string album;
    /* -1 while the sender has not said. */
    int64_t position_ms = -1;
    int64_t duration_ms = -1;
    bool has_volume = false;
    float volume_db = 0;
    /* The sender's remote control has been found; remote() is dropped until then. */
    bool remote = false;
};

enum class Command {
    PlayPause,
    Next,
    Previous,
};

NowPlaying now_playing();
void remote(Command command);

}  // namespace airplay
