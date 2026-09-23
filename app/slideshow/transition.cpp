/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "transition.hpp"
#include "slideshow_output.hpp"

#include <algorithm>
#include <cmath>

static constexpr int64_t kFadeUs = 1000000;

namespace {

uint8_t alpha_of(float weight) {
    return (uint8_t)std::clamp((int)std::lround(weight * 255.0f), 0, 255);
}

class Cut : public Transition {
public:
    int64_t duration_us() const override { return 0; }

    bool prepare(SlideshowOutput &output, const Placement &, const Placement &to) override {
        target_ = output.least_recent(output.shown());
        return output.compose(output.framebuffer(target_), to);
    }

    bool step(SlideshowOutput &, float, float) override { return true; }

    void finish(SlideshowOutput &output) override { output.present(target_); }

private:
    int target_ = -1;
};

class AccumulatingFade : public Transition {
public:
    int64_t duration_us() const override { return kFadeUs; }

    bool prepare(SlideshowOutput &output, const Placement &, const Placement &to) override {
        source_ = output.shown();
        target_ = output.least_recent(source_);
        spare_ = output.least_recent(source_, target_);
        next_ = spare_;
        return output.compose(output.framebuffer(target_), to);
    }

    bool step(SlideshowOutput &output, float progress, float previous) override {
        const uint8_t alpha = std::max<uint8_t>(alpha_of((progress - previous) / (1.0f - previous)), 1);
        const int out = next_;
        if (!output.blend(output.framebuffer(out), output.framebuffer(output.shown()),
                          output.framebuffer(target_), alpha)) {
            return false;
        }
        output.present(out);
        next_ = out == spare_ ? source_ : spare_;
        return true;
    }

    void finish(SlideshowOutput &output) override { output.present(target_); }

private:
    int source_ = -1;
    int target_ = -1;
    int spare_ = -1;
    int next_ = -1;
};

class MixingFade : public Transition {
public:
    int64_t duration_us() const override { return kFadeUs; }

    bool prepare(SlideshowOutput &output, const Placement &from, const Placement &to) override {
        if (!target_) target_ = output.allocate_frame();
        from_ = from;
        to_ = to;
        return !target_ || output.compose(target_.get(), to);
    }

    bool step(SlideshowOutput &output, float progress, float) override {
        if (!target_ || !from_.pixels) return false;
        const int out = output.least_recent(output.shown());
        uint8_t *frame = output.framebuffer(out);
        if (!output.compose(frame, from_) ||
            !output.blend(frame, frame, target_.get(), alpha_of(progress))) {
            return false;
        }
        output.present(out);
        return true;
    }

    void finish(SlideshowOutput &output) override {
        const int out = output.least_recent(output.shown());
        if (output.compose(output.framebuffer(out), to_)) output.present(out);
    }

private:
    Frame target_;
    Placement from_;
    Placement to_;
};

}

std::unique_ptr<Transition> transition_create(TransitionKind kind, const SlideshowOutput &output) {
    switch (kind) {
    case TransitionKind::Fade:
        if (output.rgb565()) return std::make_unique<MixingFade>();
        return std::make_unique<AccumulatingFade>();
    default:
        return std::make_unique<Cut>();
    }
}

float transition_ease(TransitionCurve, float t) {
    return std::clamp(t, 0.0f, 1.0f);
}
