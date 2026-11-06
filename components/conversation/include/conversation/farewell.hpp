// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <string>
#include <string_view>

namespace stackchan::conversation {
// Exact normalized utterances only: mentioning an ending word in a question
// must not terminate the session. No PCM or transcript is retained here.
inline bool is_farewell(std::string_view text) {
    if (text.empty() || text.size() > 128) return false;
    std::string normalized;
    constexpr std::string_view separators[] = {
        "。", "，", "！", "？", "；", "：", "、", "…", "　", "“", "”", "‘", "’"
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c <= 0x20 || std::string_view(".,!?;:'\"").find(static_cast<char>(c)) != std::string_view::npos) {
            ++i; continue;
        }
        bool skipped = false;
        for (const auto separator : separators) {
            if (text.substr(i).starts_with(separator)) {
                i += separator.size(); skipped = true; break;
            }
        }
        if (!skipped) {
            normalized += c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : text[i];
            ++i;
        }
    }
    constexpr std::string_view endings[] = {
        "再见", "再见了", "拜拜", "拜拜了", "退下吧", "不用了", "不需要了",
        "结束对话", "停止对话", "结束聊天", "谢谢再见", "不用了谢谢",
        "bye", "byebye", "goodbye"
    };
    for (const auto ending : endings) if (normalized == ending) return true;
    return false;
}
} // namespace stackchan::conversation
