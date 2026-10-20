// SPDX-License-Identifier: BSL-1.0
#include <cassert>
#include <cstring>
#include <memory>
#include "realtime_session.hpp"
#include <config_service/staged_config.hpp>

using namespace stackchan::conversation;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* field(const cJSON* o, const char* k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
void str(const cJSON* o, const char* k, const char* v) {
    const auto* f = field(o, k);
    assert(cJSON_IsString(f) && std::strcmp(f->valuestring, v) == 0);
}
int main() {
    ConversationConfig cfg;
    cfg.model = "grok-voice-latest";
    cfg.voice = "eve";
    cfg.instructions = "请用中文简短回答。";
    cfg.input_sample_rate_hz = 8000;
    cfg.output_sample_rate_hz = 8000;
    cfg.tools.push_back({"set_expression", "Change face", R"({"type":"object","properties":{}})"});
    Json xai(build_realtime_session(cfg, RealtimeBackend::Xai), cJSON_Delete);
    str(xai.get(), "type", "session.update");
    const auto* s = field(xai.get(), "session");
    str(s, "voice", "eve");
    str(s, "instructions", cfg.instructions.c_str());
    str(field(s, "turn_detection"), "type", "server_vad");
    assert(!field(s, "type") && !field(s, "model") && !field(s, "output_modalities"));
    const auto* audio = field(s, "audio");
    const auto* in = field(audio, "input");
    const auto* out = field(audio, "output");
    str(field(in, "format"), "type", "audio/pcmu");
    str(field(out, "format"), "type", "audio/pcmu");
    assert(!field(in, "turn_detection") && !field(in, "transcription") && !field(out, "voice"));
    const auto* tool = cJSON_GetArrayItem(field(s, "tools"), 0);
    str(tool, "name", "set_expression");
    assert(cJSON_IsObject(field(tool, "parameters")));

    // Regression: the default OpenAI dialect must retain GA's nested fields.
    cfg.model = "gpt-realtime";
    cfg.voice = "alloy";
    cfg.output_sample_rate_hz = 24000;
    Json openai(build_realtime_session(cfg, RealtimeBackend::OpenAi), cJSON_Delete);
    s = field(openai.get(), "session");
    str(s, "type", "realtime"); str(s, "model", "gpt-realtime");
    audio = field(s, "audio"); in = field(audio, "input"); out = field(audio, "output");
    str(out, "voice", "alloy");
    str(field(in, "transcription"), "model", "whisper-1");
    str(field(in, "turn_detection"), "type", "server_vad");
    str(field(out, "format"), "type", "audio/pcm");
    assert(field(field(out, "format"), "rate")->valueint == 24000);
    assert(!field(s, "voice") && !field(s, "turn_detection"));

    // Cross-transport staged settings keep all providers' keys independently.
    namespace config = stackchan::config;
    config::DeviceConfig device;
    device.openai_api_key = "openai-test";
    device.gemini_api_key = "gemini-test";
    assert(!config::registry::find("xai-api-key"));
    const auto* key = config::registry::find("xai-device-token");
    assert(key && key->secret && key->max_len == 128 && config::registry::needs_reboot(*key));
    assert(std::strcmp(key->nvs_key, "xai_dev_tok") == 0);
    const auto* provider = config::registry::find("provider");
    assert(provider && provider->max_value == 3);
    config::StagedConfig staging;
    staging.set_str("xai-device-token", "device-test");
    staging.set_num("provider", 3);
    staging.merge_into(device);
    assert(device.provider == config::Provider::Grok && device.xai_device_token == "device-test");
    assert(device.openai_api_key == "openai-test" && device.gemini_api_key == "gemini-test");
    staging.clear();
    staging.set_str("xai-device-token", "");
    staging.merge_into(device);
    assert(device.xai_device_token.empty() && device.openai_api_key == "openai-test");

    cfg.agent_id = "agent_test_only";
    cfg.instructions = "must not be sent";
    cfg.voice = "must not be sent";
    Json saved(build_realtime_session(cfg, RealtimeBackend::Xai), cJSON_Delete);
    s = field(saved.get(), "session");
    assert(cJSON_GetArraySize(s) == 1); // audio only
    assert(!field(s, "instructions") && !field(s, "voice") && !field(s, "tools") && !field(s, "model") && !field(s, "turn_detection"));
    assert(field(field(field(field(s, "audio"), "input"), "format"), "rate")->valueint == 24000);
}
