/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <memory>

class SlideshowOutput;
struct Placement;

enum class TransitionKind : uint8_t { None, Fade };
inline constexpr int kTransitionKinds = 2;

enum class TransitionCurve : uint8_t { Linear };

/* One change of picture. Which framebuffers it uses is its own choice:
   prepare() runs ahead of time with the old picture still on screen, step()
   draws and presents one frame, finish() leaves the new picture on screen. */
class Transition {
public:
    virtual ~Transition() = default;
    virtual int64_t duration_us() const = 0;
    virtual bool prepare(SlideshowOutput &output, const Placement &from, const Placement &to) = 0;
    virtual bool step(SlideshowOutput &output, float progress, float previous) = 0;
    virtual void finish(SlideshowOutput &output) = 0;
};

std::unique_ptr<Transition> transition_create(TransitionKind kind, const SlideshowOutput &output);
float transition_ease(TransitionCurve curve, float t);
