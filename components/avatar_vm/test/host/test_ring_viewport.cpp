// SPDX-License-Identifier: BSL-1.0
// Exercise the real VM, not just compositor-space centroids: on StopWatch the
// face circle is centred at (233,233), so unscaled eye coordinates are wrong.
#include "test_support.hpp"
#include "aora_face.hpp"
#include "avatar_vm/opcodes.hpp"
#include "avatar_vm/vm.hpp"
#include <cmath>

using namespace stackchan;
using namespace avatar_vm;

int main() {
    avtest::BytecodeBuilder b;
    auto op = [&](Op v) { b.code(static_cast<std::uint8_t>(v)); };
    auto num = [&](float v) { op(Op::PushF32); b.code_f32(v); };
    num(160); op(Op::Tx); num(120); op(Op::Ty); num(100); op(Op::Scale);
    num(65535); op(Op::FillCircle);
    for (int point = 0; point < 96; ++point) {
        op(Op::PushVar); b.code(static_cast<std::uint8_t>(Var::RingBase) + point * 2);
        op(Op::PushVar); b.code(static_cast<std::uint8_t>(Var::RingBase) + point * 2 + 1);
        num(1); num(0); op(Op::FillCircle);
    }
    op(Op::Ret); b.add_fn(0, 0, 0);
    const auto bytes = b.build();
    auto decoded = decode(bytes);
    CHECK(decoded.has_value());
    if (!decoded) return avtest::finish("ring viewport");
    struct Screen { int w, h; bool round; float scale; };
    // Explicit expected scale/origin for actual hardware, independent of the
    // implementation's canvas_scale helper. Includes the user's 466px circle.
    for (const auto screen : {Screen{466,466,true,1.13005f}, Screen{320,240,false,1.0f},
                              Screen{128,128,false,0.4f}, Screen{240,240,true,0.582f}}) {
        for (std::size_t expr = 0; expr < avatar::kExpressionCount; ++expr) {
            avatar::DrawContext ctx;
            ctx.fixed_gaze_enabled = true;
            ctx.expression = static_cast<avatar::Expression>(expr);
            ctx.gaze_horizontal = 1;
            ctx.gaze_vertical = -1;
            float ring[app::aora::kOutFloats];
            for (std::uint32_t now : {0u, 5000u, 12000u, 18000u}) {
                app::aora::compose(ctx, now, ring);
                ctx.aora_ring = ring;
                avtest::RecordingCanvas canvas(screen.w, screen.h, screen.round);
                avatar::FaceTuning tuning;
                Vm vm;
                CHECK(vm.run(*decoded, canvas, ctx, tuning).has_value());
                CHECK(canvas.ops.size() == 97);
                if (canvas.ops.size() != 97) continue;
                const auto& face = canvas.ops[0];
                CHECK(face.a == screen.w / 2 && face.b == screen.h / 2);
                CHECK(std::abs(face.c - 100 * screen.scale) < 1.1f);
                for (int side = 0; side < 2; ++side) {
                    float cx = 0, cy = 0;
                    for (int i = 0; i < 48; ++i) {
                        const auto& eye = canvas.ops[1 + side * 48 + i];
                        cx += eye.a; cy += eye.b;
                        const float dx = eye.a - face.a, dy = eye.b - face.b;
                        CHECK(dx*dx + dy*dy < face.c*face.c);
                        // Also prove each ring value is transformed exactly
                        // once; this catches accidental raw/double scaling.
                        const int index = side * 96 + i * 2;
                        CHECK(std::abs(eye.a - (screen.w/2.0f + (ring[index]-160)*screen.scale)) < 1.1f);
                        CHECK(std::abs(eye.b - (screen.h/2.0f + (ring[index+1]-120)*screen.scale)) < 1.1f);
                    }
                    cx /= 48; cy /= 48;
                    CHECK(std::abs(cx - (screen.w/2.0f + (side ? 32 : -32)*screen.scale)) < 1.1f);
                    CHECK(std::abs(cy - (screen.h/2.0f - 10*screen.scale)) < 1.1f);
                }
            }
        }
    }
    return avtest::finish("ring viewport");
}
