// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "avatar/expression.hpp"
#include "avatar/palette.hpp"

namespace stackchan::avatar {

struct DrawContext {
    Expression expression{Expression::Neutral};
    // Source expression for a C++-owned ease. `expression_blend` is 0 at the
    // start of a transition (fully the from-pose) and 1 when idle / done
    // (fully `expression`). Two nested holds freeze interrupted mixes so a
    // retarget does not snap. The from-pose is
    // lerp(lerp(`expression_from`, `expression_hold_to`,
    //           `expression_hold_blend`),
    //      `expression_hold2_to`, `expression_hold2_blend`).
    // `expression_hold2_blend` is 0 when the second nest is idle.
    Expression expression_from{Expression::Neutral};
    float expression_blend{1.0f};
    Expression expression_hold_to{Expression::Neutral};
    float expression_hold_blend{1.0f};
    Expression expression_hold2_to{Expression::Neutral};
    float expression_hold2_blend{0.0f};
    float breath{0.0f};
    // External / commanded gaze target — written by Avatar::set_gaze (e.g.
    // touch-follow, future API gesture). Saccade is added on top so the
    // eyes still wander while pointing roughly at the target.
    float gaze_horizontal{0.0f};
    float gaze_vertical{0.0f};
    // Animator-driven saccade offset, summed with the target above by
    // Var::GazeH / Var::GazeV in the VM. Updated by FaceAnimator::
    // saccade_tick; kept zero when saccade is disabled.
    float gaze_saccade_h{0.0f};
    float gaze_saccade_v{0.0f};
    // Suppress commanded/random gaze and recenter the built-in eye rings.
    // Expression shape, blinking, breath and mouth animation remain active.
    bool fixed_gaze_enabled{false};
    float eye_open_ratio{1.0f};
    float mouth_open_ratio{0.0f};
    Palette palette{kDefaultPalette};
    std::uint32_t rng_state{0xC0FFEEu};
    std::optional<std::string> balloon_text{};
    // Wall-clock used for time-based animation (e.g. balloon marquee).
    std::uint32_t now_ms{0};
    // First actually displayed frame starts the hold/marquee clock. The
    // avatar clock can be stale after a settings overlay paused rendering.
    std::uint32_t balloon_set_ms{0};
    bool balloon_start_pending{false};
    // Minimum display time. 0 means "use balloon defaults" (short = a fixed
    // hold, long = one marquee pass).
    std::uint32_t balloon_hold_ms{0};
    // Set by balloon rendering once the message has been displayed in full
    // (i.e. hold time elapsed for short text, or one marquee cycle for long
    // text). The render task polls this and notifies the application.
    bool balloon_done{false};
    void queue_balloon(std::string_view text, std::uint32_t hold_ms) {
        balloon_text = std::string{text};
        balloon_hold_ms = hold_ms;
        balloon_done = false;
        balloon_start_pending = true;
    }
    // aora eye-ring 320x240 design coordinates (non-owning frame-buffer pointer).
    // The VM maps each x/y to the current canvas using the same scale/origin
    // as DSL tx()/ty(), including the round-screen fit.
    // Buffer layout:
    // kRingVarCount floats: l0x,l0y..l47y,r0x..r47y). With a null pointer
    // the Var::RingBase range reads 0.
    const float* aora_ring{nullptr};
};

} // namespace stackchan::avatar
