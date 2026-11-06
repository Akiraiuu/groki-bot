// SPDX-License-Identifier: BSL-1.0
#include "test_support.hpp"
#include "balloon.hpp"
#include <cstdint>
#include <string>

using namespace stackchan::avatar;
struct Surface : RichCanvas {
    int text_width=80, strings=0, groups=0, ends=0, clips=0;
    std::int32_t width() const override{return 466;}
    std::int32_t height() const override{return 466;}
    void fillScreen(std::uint16_t) override{}
    void fillRect(std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::uint16_t) override{}
    void fillCircle(std::int32_t,std::int32_t,std::int32_t,std::uint16_t) override{}
    void fillTriangle(std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::uint16_t) override{}
    void begin_group(std::int32_t,std::int32_t,std::int32_t,std::int32_t) override{++groups;}
    void end_group() override{++ends;}
    void begin_frame(std::uint16_t) override{}
    void end_frame() override{}
    void request_full_repaint() override{}
    std::uint16_t color565(std::uint8_t,std::uint8_t,std::uint8_t) override{return 0;}
    void fillRoundRect(std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::uint16_t) override{}
    void drawRoundRect(std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::uint16_t) override{}
    void setTextColor(std::uint16_t) override{}
    void setTextColor(std::uint16_t,std::uint16_t) override{}
    void setFont(const lgfx::IFont*) override{}
    void setTextSize(float) override{}
    void setTextDatum(lgfx::textdatum_t) override{}
    void drawString(const char*,std::int32_t,std::int32_t) override{++strings;}
    std::int32_t textWidth(const char*) override{return text_width;}
    void setClipRect(std::int32_t,std::int32_t,std::int32_t,std::int32_t) override{++clips;}
    void clearClipRect() override{--clips;}
};
int main(){
    Surface c;DrawContext ctx;
    // Menu pauses avatar ticks. The new message must receive its full hold
    // on return, even when the previous avatar frame is minutes old.
    ctx.now_ms=1000;ctx.queue_balloon("ready",5000);ctx.now_ms=120000;
    internal::draw_balloon(c,ctx);CHECK(!ctx.balloon_done);CHECK(ctx.balloon_set_ms==120000);
    ctx.now_ms=124999;internal::draw_balloon(c,ctx);CHECK(!ctx.balloon_done);
    ctx.now_ms=125000;internal::draw_balloon(c,ctx);CHECK(ctx.balloon_done);
    // Replacing a completed message also resets the first-visible-frame clock.
    ctx.queue_balloon("next",3000);ctx.now_ms=300000;
    internal::draw_balloon(c,ctx);CHECK(!ctx.balloon_done);
    ctx.now_ms=303000;internal::draw_balloon(c,ctx);CHECK(ctx.balloon_done);
    // Long text gets a complete marquee pass, regardless of overlay time.
    c.text_width=800;ctx.queue_balloon("a long message",0);ctx.now_ms=600000;
    internal::draw_balloon(c,ctx);CHECK(!ctx.balloon_done);
    ctx.now_ms=610000;internal::draw_balloon(c,ctx);CHECK(!ctx.balloon_done);
    ctx.now_ms=630000;internal::draw_balloon(c,ctx);CHECK(ctx.balloon_done);
    // Unsigned timer wrap must still retain the requested display duration.
    c.text_width=80;ctx.queue_balloon("wrap",5000);ctx.now_ms=0xfffffff0;
    internal::draw_balloon(c,ctx);CHECK(!ctx.balloon_done);
    ctx.now_ms=0x10;internal::draw_balloon(c,ctx);CHECK(!ctx.balloon_done);
    ctx.now_ms=0xfffffff0u+5000u;internal::draw_balloon(c,ctx);CHECK(ctx.balloon_done);
    const auto before=c.strings;ctx.queue_balloon("",5000);internal::draw_balloon(c,ctx);CHECK(c.strings==before);
    CHECK(c.groups==c.ends);CHECK(c.clips==0);
    return avtest::finish("balloon first visible frame");
}
