// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cJSON.h>
#include <cstdint>
#include <cstring>

namespace stackchan::conversation {
enum class PaidVoiceCommand { Invalid, Start, Stop };
inline PaidVoiceCommand parse_paid_voice_command(const cJSON* root)
{
    if (!cJSON_IsObject(root)) return PaidVoiceCommand::Invalid;
    unsigned op = 0, confirm = 0, rounds = 0, seconds = 0, approval = 0;
    bool start = false, stop = false;
    for (auto* v = root->child; v != nullptr; v = v->next) {
        if (!v->string) return PaidVoiceCommand::Invalid;
        if (std::strcmp(v->string, "op") == 0) {
            ++op;
            if (!cJSON_IsString(v) || !v->valuestring) return PaidVoiceCommand::Invalid;
            start = std::strcmp(v->valuestring, "start") == 0;
            stop = std::strcmp(v->valuestring, "stop") == 0;
        } else if (std::strcmp(v->string, "confirm_paid") == 0) {
            ++confirm;
            if (!cJSON_IsTrue(v)) return PaidVoiceCommand::Invalid;
        } else if (std::strcmp(v->string, "max_rounds") == 0) {
            ++rounds;
            if (!cJSON_IsNumber(v) || v->valuedouble != 1) return PaidVoiceCommand::Invalid;
        } else if (std::strcmp(v->string, "max_seconds") == 0) {
            ++seconds;
            if (!cJSON_IsNumber(v) || v->valuedouble != 60) return PaidVoiceCommand::Invalid;
        } else if (std::strcmp(v->string, "approval_id") == 0) {
            ++approval;
            if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > UINT32_MAX ||
                static_cast<double>(static_cast<std::uint32_t>(v->valuedouble)) != v->valuedouble)
                return PaidVoiceCommand::Invalid;
        } else return PaidVoiceCommand::Invalid;
    }
    if (op != 1) return PaidVoiceCommand::Invalid;
    if (stop && confirm == 0 && rounds == 0 && seconds == 0 && approval == 0) return PaidVoiceCommand::Stop;
    if (start && confirm == 1 && rounds == 1 && seconds == 1 && approval == 1) return PaidVoiceCommand::Start;
    return PaidVoiceCommand::Invalid;
}
} // namespace stackchan::conversation
