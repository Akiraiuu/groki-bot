// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <tl/expected.hpp>
#include "conversation/conversation_error.hpp"
#include "conversation/connect_diagnostics.hpp"

namespace stackchan::conversation {
inline constexpr const char* kSavedAgentTokenUrl =
    "https://stopwatch-agent-token-p1-20261020.stopwatch-agent-token-service.workers.dev/v1/session";
struct AgentTicket {
    std::string token;
    std::string agent_id;
};
struct TicketPreparationPermit {
    std::int64_t deadline_us{}; // Original approval end, including preparation.
    std::function<bool()> allowed;
    std::shared_ptr<void> operation_lease; // Retained on uncertain cleanup.
    std::uint32_t generation{}; // Current grant identity; one attempt per grant.
    ConnectObserver connect_observer;
};
// Exactly one scoped, certificate-verified HTTPS request. No retry/NVS writes.
// The allowed closure must own/pin its context through actual cleanup.
tl::expected<AgentTicket, ConversationError> fetch_agent_ticket(
    const std::string& url, const std::string& device_token, const TicketPreparationPermit& permit);
}
