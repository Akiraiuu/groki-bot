// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cJSON.h>
#include "conversation/openai_realtime_client.hpp"

namespace stackchan::conversation {

// Caller owns the returned JSON. xAI shares the streaming event protocol but
// uses top-level voice / turn_detection and no OpenAI session type or model.
// https://docs.x.ai/developers/model-capabilities/audio/speech-to-speech
inline cJSON* build_realtime_session(const ConversationConfig& config, RealtimeBackend backend)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON* session = cJSON_AddObjectToObject(root, "session");
    if (backend == RealtimeBackend::Xai && !config.agent_id.empty()) {
        cJSON* audio = cJSON_AddObjectToObject(session, "audio");
        for (const char* side : {"input", "output"}) {
            cJSON* io = cJSON_AddObjectToObject(audio, side);
            cJSON* format = cJSON_AddObjectToObject(io, "format");
            cJSON_AddStringToObject(format, "type", "audio/pcm");
            cJSON_AddNumberToObject(format, "rate", 24000);
        }
        return root; // Never overwrite the saved Agent's configuration.
    }
    const bool xai = backend == RealtimeBackend::Xai;
    if (!xai) {
        cJSON_AddStringToObject(session, "type", "realtime");
        if (!config.model.empty()) cJSON_AddStringToObject(session, "model", config.model.c_str());
        cJSON* mods = cJSON_AddArrayToObject(session, "output_modalities");
        cJSON_AddItemToArray(mods, cJSON_CreateString("audio"));
    }

    if (!config.instructions.empty()) {
        cJSON_AddStringToObject(session, "instructions", config.instructions.c_str());
    }

    // ---- audio.input / audio.output ----
    cJSON* audio = cJSON_AddObjectToObject(session, "audio");

    cJSON* in = cJSON_AddObjectToObject(audio, "input");
    cJSON* in_fmt = cJSON_AddObjectToObject(in, "format");
    // Mic uplink is G.711 µ-law @ 8 kHz — 6× lighter on the wire than
    // pcm16 @ 24 kHz, which made the audio path immune to coex /
    // DTIM stalls eating its TCP send buffer. The mic side encodes
    // PCM16 → µ-law in push_audio (see pcm16_to_ulaw). OpenAI Realtime
    // accepts pcmu only at 8 kHz, so no rate field.
    cJSON_AddStringToObject(in_fmt, "type", "audio/pcmu");

    cJSON* vad = cJSON_AddObjectToObject(xai ? session : in, "turn_detection");
    cJSON_AddStringToObject(vad, "type", "server_vad");
    if (!xai) {
        cJSON_AddNumberToObject(vad, "threshold", config.vad_threshold);
        cJSON_AddNumberToObject(vad, "prefix_padding_ms", config.vad_prefix_padding_ms);
        cJSON_AddNumberToObject(vad, "silence_duration_ms", config.vad_silence_ms);
    }

    if (!xai && config.enable_input_transcription) {
        cJSON* tr = cJSON_AddObjectToObject(in, "transcription");
        cJSON_AddStringToObject(tr, "model", "whisper-1");
    }

    cJSON* out = cJSON_AddObjectToObject(audio, "output");
    cJSON* out_fmt = cJSON_AddObjectToObject(out, "format");
    if (config.output_sample_rate_hz == 8000) {
        cJSON_AddStringToObject(out_fmt, "type", "audio/pcmu"); // G.711 µ-law @ 8 kHz
    } else {
        cJSON_AddStringToObject(out_fmt, "type", "audio/pcm");
        cJSON_AddNumberToObject(out_fmt, "rate", config.output_sample_rate_hz);
    }
    if (!config.voice.empty()) {
        cJSON_AddStringToObject(xai ? session : out, "voice", config.voice.c_str());
    }

    // ---- tools ----
    cJSON* tools = cJSON_AddArrayToObject(session, "tools");
    for (const auto& tool : config.tools) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(t, "name", tool.name.c_str());
        cJSON_AddStringToObject(t, "description", tool.description.c_str());
        cJSON* params = cJSON_Parse(tool.parameters_json.c_str());
        if (params != nullptr) {
            cJSON_AddItemToObject(t, "parameters", params);
        }
        cJSON_AddItemToArray(tools, t);
    }
    if (!xai) cJSON_AddStringToObject(session, "tool_choice", "auto");
    return root;
}

} // namespace stackchan::conversation
