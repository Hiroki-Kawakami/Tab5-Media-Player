/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "transition.hpp"
#include "slideshow_output.hpp"

#include <algorithm>
#include <cmath>

static constexpr int64_t kDurationUs = 1000000;

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
    int64_t duration_us() const override { return kDurationUs; }

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
    int64_t duration_us() const override { return kDurationUs; }

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

class Wipe : public Transition {
public:
    explicit Wipe(TransitionDirection direction) : direction_(direction) {}

    int64_t duration_us() const override { return kDurationUs; }

    bool prepare(SlideshowOutput &output, const Placement &, const Placement &to) override {
        to_ = to;
        const int shown = output.shown();
        copied_ = true;
        for (int i = 0; i < SlideshowOutput::kFramebuffers; i++) {
            drawn_[i] = 0;
            if (i != shown) copied_ &= output.copy(output.framebuffer(i), output.framebuffer(shown));
        }
        return true;
    }

    bool step(SlideshowOutput &output, float progress, float) override {
        if (!copied_) return false;
        return reveal(output, (int)std::lround(progress * (float)length(output)));
    }

    void finish(SlideshowOutput &output) override {
        if (!copied_) {
            for (int &drawn : drawn_) drawn = 0;
        }
        reveal(output, length(output));
    }

private:
    bool horizontal() const {
        return direction_ == TransitionDirection::LeftToRight ||
               direction_ == TransitionDirection::RightToLeft;
    }

    int length(const SlideshowOutput &output) const {
        const bsp_size_t screen = output.screen();
        return horizontal() ? screen.width : screen.height;
    }

    bsp_rect_t band(const SlideshowOutput &output, int from, int to) const {
        const bsp_size_t screen = output.screen();
        switch (direction_) {
        case TransitionDirection::RightToLeft:
            return { { screen.width - to, 0 }, { to - from, screen.height } };
        case TransitionDirection::TopToBottom:
            return { { 0, from }, { screen.width, to - from } };
        case TransitionDirection::BottomToTop:
            return { { 0, screen.height - to }, { screen.width, to - from } };
        default:
            return { { from, 0 }, { to - from, screen.height } };
        }
    }

    bool reveal(SlideshowOutput &output, int extent) {
        const int out = output.least_recent(output.shown());
        if (extent > drawn_[out] &&
            !output.draw_region(output.framebuffer(out), to_, band(output, drawn_[out], extent))) {
            return false;
        }
        drawn_[out] = std::max(drawn_[out], extent);
        output.present(out);
        return true;
    }

    TransitionDirection direction_;
    Placement to_;
    int drawn_[SlideshowOutput::kFramebuffers] = {};
    bool copied_ = false;
};
}

std::unique_ptr<Transition> transition_create(TransitionKind kind, TransitionDirection direction,
                                              const SlideshowOutput &output) {
    switch (kind) {
    case TransitionKind::Fade:
        if (output.rgb565()) return std::make_unique<MixingFade>();
        return std::make_unique<AccumulatingFade>();
    case TransitionKind::Wipe:
        return std::make_unique<Wipe>(direction);
    default:
        return std::make_unique<Cut>();
    }
}

bool transition_has_direction(TransitionKind kind) {
    return kind == TransitionKind::Wipe;
}

float transition_ease(TransitionCurve, float t) {
    return std::clamp(t, 0.0f, 1.0f);
}
