// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "voice_control_policy.hpp"

namespace stopwatch::candidate {
template<class PaidTest>
bool stop_on_inbox_failure(FaultSnapshot first, PaidTest& test) {
    if (first.kind == DeliveryFault::None) return false;
    using Reason = typename PaidTest::Reason;
    using Fault = typename PaidTest::Fault;
    if (first.kind == DeliveryFault::SourceError)
        test.request_stop(Reason::Error, Fault::ConversationEvent, first.source_error);
    else
        test.request_stop(Reason::Error, Fault::EventInbox);
    return true;
}
} // namespace stopwatch::candidate
