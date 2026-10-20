// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cmath>
#include <cstring>
#include <string_view>
#include <cJSON.h>
#include "conversation/saved_agent.hpp"

namespace stackchan::conversation {
inline bool base64url_id(std::string_view value, std::size_t min, std::size_t max) {
    if (value.size() < min || value.size() > max) return false;
    for (unsigned char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}
inline bool valid_saved_agent_id(std::string_view id) {
    return id.starts_with("agent_") && base64url_id(id.substr(6), 1, 120);
}
inline const char* agent_json_string(const cJSON* obj, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}
inline tl::expected<AgentTicket, ConversationError> parse_agent_ticket(
        const char* json, std::size_t length, double now) {
    if (length == 0 || length > 8192) return tl::unexpected{ConversationError::ProtocolError};
    auto* root = cJSON_ParseWithLength(json, length);
    if (!root) return tl::unexpected{ConversationError::ProtocolError};
    const auto* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const auto* expiry = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    const char* token = agent_json_string(root, "token");
    const char* agent = agent_json_string(root, "agent_id");
    const char* url = agent_json_string(root, "websocket_url");
    bool valid = cJSON_IsNumber(version) && version->valuedouble == 1 &&
        cJSON_IsNumber(expiry) && std::isfinite(expiry->valuedouble) &&
        expiry->valuedouble > now + 15 && expiry->valuedouble <= now + 330 &&
        token && std::strlen(token) >= 8 && std::strlen(token) <= 4096 &&
        agent && valid_saved_agent_id(agent) && url &&
        std::string(url) == std::string("wss://api.x.ai/v1/realtime?agent_id=") + agent;
    if (token) for (const unsigned char* c = reinterpret_cast<const unsigned char*>(token); *c; ++c) {
        if (*c <= 32 || *c >= 127) valid = false;
    }
    AgentTicket ticket;
    if (valid) { ticket.token = token; ticket.agent_id = agent; }
    cJSON_Delete(root);
    if (!valid) return tl::unexpected{ConversationError::ProtocolError};
    return ticket;
}
inline bool saved_agent_audio_confirmed(const cJSON* session) {
    const auto* audio = cJSON_GetObjectItemCaseSensitive(session, "audio");
    for (const char* side : {"input", "output"}) {
        const auto* io = cJSON_GetObjectItemCaseSensitive(audio, side);
        const auto* format = cJSON_GetObjectItemCaseSensitive(io, "format");
        const char* transport = agent_json_string(io, "transport");
        if (transport && std::strcmp(transport, "json") != 0) return false;
        const char* type = agent_json_string(format, "type");
        const auto* rate = cJSON_GetObjectItemCaseSensitive(format, "rate");
        if (!type || std::strcmp(type, "audio/pcm") != 0 || !cJSON_IsNumber(rate) || rate->valuedouble != 24000) return false;
        for (const auto* object : {io, format}) {
            for (const char* key : {"channels", "channel_count"}) {
                const auto* channels = cJSON_GetObjectItemCaseSensitive(object, key);
                if (channels && (!cJSON_IsNumber(channels) || channels->valuedouble != 1)) return false;
            }
        }
    }
    return true;
}
}
