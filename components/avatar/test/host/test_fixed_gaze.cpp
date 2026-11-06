// SPDX-License-Identifier: BSL-1.0
#include "test_support.hpp"
#include "aora_face.hpp"
#include "animation.hpp"
#include "config_service/settings_registry.hpp"
#include "config_service/staged_config.hpp"
#include <algorithm>
#include <cmath>

using namespace stackchan;

namespace {
float center(const float* points, std::size_t side, std::size_t axis) {
    float sum = 0;
    for (std::size_t i = 0; i < app::aora::kRingPoints; ++i)
        sum += points[side * app::aora::kRingPoints * 2 + i * 2 + axis];
    return sum / app::aora::kRingPoints;
}
float height(const float* points) {
    float lo = points[1], hi = points[1];
    for (std::size_t i = 1; i < app::aora::kRingPoints * 2; i += 2) {
        lo = std::min(lo, points[i]);
        hi = std::max(hi, points[i]);
    }
    return hi - lo;
}
bool near(float a, float b) { return std::fabs(a - b) < 0.002f; }
}

int main() {
    // Every expression, including interrupted blends and built-in orbit/pool
    // changes, must remain centered despite nonzero touch/tilt/saccade inputs.
    avatar::DrawContext ctx;
    ctx.fixed_gaze_enabled = true;
    ctx.gaze_horizontal = 0.9f;
    ctx.gaze_vertical = -0.8f;
    ctx.gaze_saccade_h = 0.7f;
    ctx.gaze_saccade_v = -0.6f;
    float points[app::aora::kOutFloats];
    for (std::size_t expr = 0; expr < avatar::kExpressionCount; ++expr) {
        ctx.expression = static_cast<avatar::Expression>(expr);
        for (float blend : {0.0f, 0.35f, 1.0f}) {
            ctx.expression_from = avatar::Expression::Sad;
            ctx.expression_hold_to = avatar::Expression::Happy;
            ctx.expression_hold2_to = avatar::Expression::Affection;
            ctx.expression_blend = blend;
            ctx.expression_hold_blend = 0.4f;
            ctx.expression_hold2_blend = 0.2f;
            for (std::uint32_t time : {0u, 850u, 1550u, 12000u, 12500u, 18000u, 60000u}) {
                app::aora::compose(ctx, time, points);
                for (std::size_t side = 0; side < 2; ++side) {
                    CHECK(near(center(points, side, 0), side == 0 ? 128.0f : 192.0f));
                    CHECK(near(center(points, side, 1), 110.0f));
                }
                for (float point : points) CHECK(std::isfinite(point));
            }
        }
    }

    // Fixed gaze still blinks and preserves expression shapes, rather than
    // freezing a captured frame. Closing the lids must change the contour.
    ctx.expression = avatar::Expression::Neutral;
    ctx.expression_blend = 1;
    ctx.eye_open_ratio = 1;
    app::aora::compose(ctx, 5000, points);
    const float open_height = height(points);
    ctx.eye_open_ratio = 0;
    app::aora::compose(ctx, 5000, points);
    CHECK(height(points) < open_height * 0.2f);
    CHECK(near(center(points, 0, 1), 110.0f));
    ctx.eye_open_ratio = 1;
    ctx.expression = avatar::Expression::Happy;
    app::aora::compose(ctx, 5000, points);
    CHECK(std::fabs(height(points) - open_height) > 0.1f);

    // Turning it off restores the original side look; turning it back on
    // recenters immediately, even before another animator update.
    ctx.expression = avatar::Expression::Neutral;
    ctx.fixed_gaze_enabled = false;
    app::aora::compose(ctx, 5000, points);
    CHECK(!near(center(points, 0, 0), 128.0f));
    ctx.fixed_gaze_enabled = true;
    app::aora::compose(ctx, 5000, points);
    CHECK(near(center(points, 0, 0), 128.0f));

    avatar::internal::FaceAnimator animator;
    animator.tick(5000, ctx);
    CHECK(ctx.gaze_saccade_h == 0 && ctx.gaze_saccade_v == 0);
    const float breath = ctx.breath;
    const float mouth = ctx.mouth_open_ratio = 0.75f;
    animator.tick(5100, ctx);
    CHECK(ctx.breath != breath);
    CHECK(ctx.mouth_open_ratio == mouth);
    ctx.fixed_gaze_enabled = false;
    animator.tick(5200, ctx);
    CHECK(ctx.gaze_saccade_h != 0 || ctx.gaze_saccade_v != 0);

    // Upgrade default and registry round-trip: opt-out is a real saved boolean,
    // and unrelated staged settings cannot overwrite this immediate setting.
    config::DeviceConfig cfg;
    CHECK(cfg.fixed_gaze_enabled);
    const auto* setting = config::registry::find("fixed-gaze");
    CHECK(setting != nullptr);
    if (setting) {
        CHECK(setting->apply == config::registry::ApplyKind::Immediate);
        CHECK(!config::registry::needs_reboot(*setting));
        CHECK(config::registry::find_by_nvs_key("fixed_gaze") == setting);
        setting->num_set(cfg, 0);
        CHECK(!cfg.fixed_gaze_enabled && setting->num_get(cfg) == 0);
        config::StagedConfig staging;
        staging.set_str("device-name", "test");
        staging.merge_into(cfg);
        CHECK(!cfg.fixed_gaze_enabled);
        setting->num_set(cfg, 1);
        CHECK(cfg.fixed_gaze_enabled && setting->num_get(cfg) == 1);
    }
    return avtest::finish("fixed gaze");
}
