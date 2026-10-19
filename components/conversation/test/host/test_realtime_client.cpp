// SPDX-License-Identifier: BSL-1.0
#include <algorithm>
#include <cassert>
#include "host_transport.hpp"
#include "conversation/openai_realtime_client.hpp"

using namespace stackchan::conversation;
int main() {
    OpenAiRealtimeClient client("xai-test-only", RealtimeBackend::Xai);
    std::vector<ConversationEvent> events;
    client.set_event_callback([&](const auto& ev) { events.push_back(ev); });
    ConversationConfig cfg;
    cfg.model = "grok-voice-latest"; cfg.voice = "eve";
    cfg.input_sample_rate_hz = cfg.output_sample_rate_hz = 8000;
    assert(client.start(cfg));
    assert(host_socket->uri == "wss://api.x.ai/v1/realtime?model=grok-voice-latest");
    assert(host_socket->headers.at("Authorization") == "Bearer xai-test-only");
    host_connected();
    assert(host_socket->sent.size() == 1);
    host_receive(R"({"type":"session.created"})");
    assert(client.state() == ConversationState::Connecting);
    host_receive(R"({"type":"session.updated"})");
    assert(client.state() == ConversationState::Listening);
    host_receive(R"({"type":"input_audio_buffer.speech_stopped"})");
    assert(client.state() == ConversationState::Thinking);
    host_receive(R"({"type":"response.created"})");
    host_receive(R"({"type":"response.output_audio.delta","delta":"/w=="})");
    assert(client.state() == ConversationState::Speaking);
    assert(events.back().type == ConversationEventType::AssistantAudioChunk);
    assert(events.back().audio->size() == 1 && events.back().audio->at(0) == 0);
    assert(client.cancel_response());
    assert(client.state() == ConversationState::Listening);
    assert(host_socket->sent.back() == R"({"type":"response.cancel"})");
    auto event_count = events.size();
    // Old audio, transcripts and tool calls must not revive an abandoned turn.
    host_receive(R"({"type":"response.output_audio.delta","delta":"/w=="})");
    host_receive(R"({"type":"response.output_audio_transcript.delta","delta":"old"})");
    host_receive(R"({"type":"response.function_call_arguments.done","call_id":"old","name":"set_expression","arguments":"{}"})");
    host_receive(R"({"type":"response.done"})");
    assert(events.size() == event_count);
    // A new turn works normally after interruption.
    host_receive(R"({"type":"response.created"})");
    host_receive(R"({"type":"response.output_audio.delta","delta":"/w=="})");
    assert(events.back().type == ConversationEventType::AssistantAudioChunk);
    host_receive(R"({"type":"response.done"})");
    auto sent = host_socket->sent.size();
    assert(client.cancel_response());
    assert(host_socket->sent.size() == sent); // already finished: no spurious cancel
    host_receive(R"({"type":"error","error":{"message":"network test"}})");
    assert(events.back().type == ConversationEventType::Error); // errors remain visible
    client.stop();
    assert(client.state() == ConversationState::Idle);
    // Reconnection resets the cancellation gate and sends credentials again.
    assert(client.start(cfg)); host_connected();
    host_receive(R"({"type":"session.updated"})");
    host_receive(R"({"type":"response.created"})");
    host_receive(R"({"type":"response.audio.delta","delta":"/w=="})");
    assert(events.back().type == ConversationEventType::AssistantAudioChunk);
    client.stop();

    // Default constructor retains the upstream OpenAI endpoint.
    OpenAiRealtimeClient openai("openai-test-only"); cfg.model = "gpt-realtime";
    assert(openai.start(cfg));
    assert(host_socket->uri == "wss://api.openai.com/v1/realtime?model=gpt-realtime");
    openai.stop();
}
