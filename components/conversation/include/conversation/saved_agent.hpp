// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <string>
#include <tl/expected.hpp>
#include "conversation/conversation_error.hpp"

namespace stackchan::conversation {
inline constexpr const char* kSavedAgentTokenUrl =
    "https://stopwatch-agent-token-p1-20261020.stopwatch-agent-token-service.workers.dev/v1/session";
struct AgentTicket {
    std::string token;
    std::string agent_id;
};
// Each attempt performs a new, certificate-verified HTTPS request. No NVS writes.
tl::expected<AgentTicket, ConversationError> fetch_agent_ticket(
    const std::string& url, const std::string& device_token);
}
