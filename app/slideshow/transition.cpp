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

class Sweep : public Transition {
public:
    Sweep(TransitionDirection direction, bool target_moves, bool source_moves)
        : direction_(direction), target_moves_(target_moves), source_moves_(source_moves) {}

    int64_t duration_us() const override { return kDurationUs; }

    bool prepare(SlideshowOutput &output, const Placement &, const Placement &to) override {
        to_ = to;
        shown_extent_ = 0;
        for (int &drawn : drawn_) drawn = 0;
        ready_ = true;
        if (source_moves_) return true;
        const int shown = output.shown();
        for (int i = 0; i < SlideshowOutput::kFramebuffers; i++) {
            if (i != shown) ready_ &= output.copy(output.framebuffer(i), output.framebuffer(shown));
        }
        return true;
    }

    bool step(SlideshowOutput &output, float progress, float) override {
        if (!ready_) return false;
        return reveal(output, (int)std::lround(progress * (float)length(output)));
    }

    void finish(SlideshowOutput &output) override {
        if (!ready_) {
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

    bsp_point_t moved(bsp_point_t point, int distance) const {
        switch (direction_) {
        case TransitionDirection::RightToLeft: return { point.x - distance, point.y };
        case TransitionDirection::TopToBottom: return { point.x, point.y + distance };
        case TransitionDirection::BottomToTop: return { point.x, point.y - distance };
        default: return { point.x + distance, point.y };
        }
    }

    bool reveal(SlideshowOutput &output, int extent) {
        const int total = length(output);
        const int out = output.least_recent(output.shown());
        uint8_t *frame = output.framebuffer(out);

        if (source_moves_ && extent < total) {
            const bsp_rect_t to = band(output, extent, total);
            bsp_rect_t from = to;
            from.origin = moved(to.origin, shown_extent_ - extent);
            if (!output.copy_region(frame, to.origin, output.framebuffer(output.shown()), from)) {
                return false;
            }
        }

        const int drawn = target_moves_ ? 0 : drawn_[out];
        if (extent > drawn) {
            Placement placement = to_;
            if (target_moves_) placement.rect.origin = moved(to_.rect.origin, extent - total);
            if (!output.draw_region(frame, placement, band(output, drawn, extent))) return false;
        }
        drawn_[out] = std::max(drawn_[out], extent);
        output.present(out);
        shown_extent_ = extent;
        return true;
    }

    TransitionDirection direction_;
    bool target_moves_;
    bool source_moves_;
    Placement to_;
    int drawn_[SlideshowOutput::kFramebuffers] = {};
    int shown_extent_ = 0;
    bool ready_ = false;
};
}

std::unique_ptr<Transition> transition_create(TransitionKind kind, TransitionDirection direction,
                                              const SlideshowOutput &output) {
    switch (kind) {
    case TransitionKind::Fade:
        if (output.rgb565()) return std::make_unique<MixingFade>();
        return std::make_unique<AccumulatingFade>();
    case TransitionKind::Wipe:
        return std::make_unique<Sweep>(direction, false, false);
    case TransitionKind::SlideIn:
        return std::make_unique<Sweep>(direction, true, false);
    case TransitionKind::SlideOut:
        return std::make_unique<Sweep>(direction, false, true);
    default:
        return std::make_unique<Cut>();
    }
}

bool transition_has_direction(TransitionKind kind) {
    return kind == TransitionKind::Wipe || kind == TransitionKind::SlideIn ||
           kind == TransitionKind::SlideOut;
}

float transition_ease(TransitionCurve, float t) {
    return std::clamp(t, 0.0f, 1.0f);
}
