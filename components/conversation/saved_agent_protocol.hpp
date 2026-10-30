// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <cJSON.h>
#include "conversation/saved_agent.hpp"

namespace stackchan::conversation {
// IMF-fixdate from the certificate-verified broker response; independent of TZ.
// Never use an HTTP Date from a redirect or a failed TLS/HTTP request.
inline std::optional<std::int64_t> saved_agent_http_date(std::string_view date) {
    if (date.size() != 29 || date[3] != ',' || date[4] != ' ' || date[7] != ' ' ||
        date[11] != ' ' || date[16] != ' ' || date[19] != ':' || date[22] != ':' ||
        date.substr(25) != " GMT") return std::nullopt;
    auto number = [&](std::size_t offset, std::size_t length) {
        int result = 0;
        for (std::size_t i = offset; i < offset + length; ++i) {
            if (date[i] < '0' || date[i] > '9') return -1;
            result = result * 10 + date[i] - '0';
        }
        return result;
    };
    constexpr std::string_view months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    constexpr std::string_view weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int month = -1, weekday = -1;
    for (int i = 0; i < 12; ++i) if (date.substr(8, 3) == months[i]) month = i;
    for (int i = 0; i < 7; ++i) if (date.substr(0, 3) == weekdays[i]) weekday = i;
    const int year = number(12, 4), day = number(5, 2), hour = number(17, 2), minute = number(20, 2), second = number(23, 2);
    if (year < 2025 || year > 2099 || month < 0 || weekday < 0 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
    auto leap = [](int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); };
    constexpr int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (day > month_days[month] + (month == 1 && leap(year) ? 1 : 0)) return std::nullopt;
    std::int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += leap(y) ? 366 : 365;
    for (int m = 0; m < month; ++m) days += month_days[m] + (m == 1 && leap(year) ? 1 : 0);
    days += day - 1;
    if ((days + 4) % 7 != weekday) return std::nullopt;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}
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
