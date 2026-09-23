/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <memory>

class SlideshowOutput;
struct Placement;

enum class TransitionKind : uint8_t { None, Fade, Wipe, SlideIn, SlideOut, Push };
inline constexpr int kTransitionKinds = 6;

/* The way a transition moves across the screen, as the viewer sees it. */
enum class TransitionDirection : uint8_t { LeftToRight, RightToLeft, TopToBottom, BottomToTop };
inline constexpr int kTransitionDirections = 4;

enum class TransitionCurve : uint8_t { Linear };

/* One change of picture. Which framebuffers it uses is its own choice:
   prepare() runs ahead of time with the old picture still on screen, step()
   draws and presents one frame, finish() leaves the new picture on screen. */
class Transition {
public:
    virtual ~Transition() = default;
    virtual int64_t duration_us() const = 0;
    /* Where the new picture is read before prepare(), a panel-sized buffer.
       Nothing may be presented in between. */
    virtual uint8_t *pixels_buffer(SlideshowOutput &output) = 0;
    virtual bool prepare(SlideshowOutput &output, const Placement &to) = 0;
    virtual bool step(SlideshowOutput &output, float progress, float previous) = 0;
    virtual void finish(SlideshowOutput &output) = 0;
};

/* Null if the buffer the transition needs cannot be allocated. */
std::unique_ptr<Transition> transition_create(TransitionKind kind, TransitionDirection direction,
                                              const SlideshowOutput &output);
bool transition_has_direction(TransitionKind kind);
float transition_ease(TransitionCurve curve, float t);
