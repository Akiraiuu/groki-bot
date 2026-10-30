// SPDX-License-Identifier: BSL-1.0
#include "conversation/wake_confirmation.hpp"
#include <cassert>
#include <thread>

using namespace stackchan::conversation;
int main() {
    PaidVoiceTest p;
    WakeConfirmation w;
    assert(!w.offer(0, p));
    p.set_available(true, 123);
    assert(w.offer(100, p));
    auto shown = w.snapshot(100);
    assert(shown.id && shown.remaining_ms == 20000);
    // Wake, stale UI clicks and even claims never authorize a connection.
    assert(p.snapshot(100).phase == PaidVoiceTest::Phase::Idle);
    assert(!p.permitted(100) && !p.claim(100));
    assert(!w.confirm(101, 0, p));
    assert(!w.confirm(101, shown.id + 1, p));
    assert(w.confirm(102, shown.id, p));
    assert(!w.confirm(103, shown.id, p));
    assert(!w.offer(103, p));
    assert(p.claim(103) && p.permitted(103));
    assert(p.snapshot(103).remaining_ms == 59999);
    assert(!p.permitted(60102));
    p.finish();
    assert(w.offer(70000, p));
    shown = w.snapshot(70000);
    assert(!w.confirm(90000, shown.id, p)); // exact expiry boundary
    assert(!p.claim(90000) && !p.permitted(90000));
    assert(w.offer(91000, p));
    shown = w.snapshot(91000);
    w.cancel();
    assert(!w.confirm(91001, shown.id, p));
    assert(w.offer(91002, p));
    assert(!w.confirm(91003, shown.id, p)); // old rendered generation
    shown = w.snapshot(91004);
    p.request_stop(); // idle stop revokes the captured paid nonce
    assert(!w.confirm(91005, shown.id, p));
    assert(!p.claim(91005));
    assert(w.offer(92000, p));
    shown = w.snapshot(92000);
    // Local diagnostic/start from another surface makes a dialog obsolete.
    const auto id = p.snapshot(92000).approval_id;
    assert(p.request_start(true, 92001, id, PaidVoiceTest::Mode::Local));
    assert(!w.confirm(92002, shown.id, p));
    assert(!p.claim(92003));
    assert(p.claim(92003, PaidVoiceTest::Mode::Local));
    assert(!p.permitted(92003) && p.local_permitted(92003));
    p.request_stop(); p.finish();
    assert(w.offer(100000, p));
    shown = w.snapshot(100000);
    // Two input tasks cannot consume a dialog twice.
    bool a = false, b = false;
    std::thread t1([&] { a = w.confirm(100001, shown.id, p); });
    std::thread t2([&] { b = w.confirm(100001, shown.id, p); });
    t1.join(); t2.join();
    assert(a != b);
    assert(p.claim(100002));
    p.request_stop(); p.finish();
    assert(!w.confirm(100003, shown.id, p));
    assert(w.offer(110000, p));
    p.set_available(false);
    assert(!w.confirm(110001, w.snapshot(110001).id, p));
    // The scope shown by the current prompt is exactly the scope granted.
    PaidVoiceTest dialogue;
    WakeConfirmation prompt;
    dialogue.set_available(true, 1234);
    assert(prompt.offer(0, dialogue, PaidVoiceTest::Profile::Dialogue));
    auto scope = prompt.snapshot(1);
    assert(scope.max_rounds == 3 && scope.idle_timeout_ms == 10000);
    assert(!dialogue.permitted(1) && !dialogue.claim(1));
    assert(!prompt.confirm(2, scope.id + 1, dialogue));
    assert(prompt.confirm(3, scope.id, dialogue));
    assert(!prompt.confirm(4, scope.id, dialogue));
    assert(dialogue.snapshot(4).max_rounds == 3);
    assert(dialogue.snapshot(4).remaining_ms == 59999);
    assert(dialogue.claim(5));
    dialogue.request_stop();
    dialogue.finish();

}
