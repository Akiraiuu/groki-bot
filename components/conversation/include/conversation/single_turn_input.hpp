// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "conversation/conversation_service.hpp"

namespace stackchan::conversation {

// A VAD boundary can be a short power/noise transient, with no committed
// utterance or transcript. Only a nonblank recognized utterance consumes input.
class SingleTurnInput {
public:
    void reset() { recognized_ = false; }
    void observe(const ConversationEvent& event) {
        if (event.type != ConversationEventType::UserTranscript) return;
        if (event.text.find_first_not_of(" \t\r\n\f\v") != std::string::npos) recognized_ = true;
    }
    bool open() const { return !recognized_; }
private:
    bool recognized_{false};
};

} // namespace stackchan::conversation
