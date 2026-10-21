// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <algorithm>
#include <cassert>
#include <ctime>
#include <cstring>
#include "host_transport.hpp"
#include "esp_http_client.h"
#include "conversation/openai_realtime_client.hpp"
#include "saved_agent_protocol.hpp"

using namespace stackchan::conversation;
std::string ticket_json(std::string id = "agent_test_only", double expiry = std::time(nullptr) + 300) {
    auto* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "token", "fake-ephemeral-token");
    cJSON_AddNumberToObject(root, "expires_at", expiry);
    cJSON_AddStringToObject(root, "agent_id", id.c_str());
    cJSON_AddStringToObject(root, "websocket_url", ("wss://api.x.ai/v1/realtime?agent_id=" + id).c_str());
    char* text = cJSON_PrintUnformatted(root);
    std::string result(text); cJSON_free(text); cJSON_Delete(root); return result;
}
const std::string confirmed = R"({"type":"session.updated","session":{"audio":{"input":{"format":{"type":"audio/pcm","rate":24000}},"output":{"format":{"type":"audio/pcm","rate":24000}}},"tools":[{"type":"web_search"},{"type":"x_search"}]}})";
void receive_fragmented(std::string json, std::size_t step = 8192) {
    for (std::size_t offset = 0; offset < json.size(); offset += step) {
        esp_websocket_event_data_t data{};
        data.data_len = static_cast<int>(std::min(step, json.size() - offset));
        data.payload_len = static_cast<int>(json.size());
        data.payload_offset = static_cast<int>(offset);
        data.op_code = 1;
        data.data_ptr = json.data() + offset;
        host_socket->callback(host_socket->arg, nullptr, WEBSOCKET_EVENT_DATA, &data);
    }
}
int main() {
    auto* audio_event = cJSON_Parse(confirmed.c_str());
    auto* audio_session = cJSON_GetObjectItemCaseSensitive(audio_event, "session");
    assert(saved_agent_audio_confirmed(audio_session));
    auto* input = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(audio_session, "audio"), "input");
    cJSON_AddNumberToObject(input, "channels", 2);
    assert(!saved_agent_audio_confirmed(audio_session));
    cJSON_DeleteItemFromObjectCaseSensitive(input, "channels");
    cJSON_AddStringToObject(input, "transport", "binary");
    assert(!saved_agent_audio_confirmed(audio_session));
    cJSON_Delete(audio_event);
    const auto now = std::time(nullptr);
    auto json = ticket_json();
    assert(parse_agent_ticket(json.data(), json.size(), now));
    for (auto bad : {ticket_json("agent_bad&other=1"), ticket_json("agent_test_only", now - 1), ticket_json("agent_test_only", now + 3600), std::string("{}")}) {
        assert(!parse_agent_ticket(bad.data(), bad.size(), now));
    }
    host_http_body = json;
    auto ticket = fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd'));
    assert(ticket && ticket->agent_id == "agent_test_only");
    assert(host_http.config.disable_auto_redirect && host_http.config.max_authorization_retries == -1);
    assert(host_http.config.crt_bundle_attach && host_http.config.timeout_ms == 15000);
    assert(host_http_authorization == "Bearer " + std::string(43, 'd'));
    assert(host_http_cleanups == 1);
    const auto calls = host_http_perform_calls;
    assert(!fetch_agent_ticket("https://elsewhere.example", std::string(43, 'd')));
    assert(!fetch_agent_ticket(kSavedAgentTokenUrl, "xai-long-term-key"));
    assert(host_http_perform_calls == calls);
    host_http_status = 302;
    assert(!fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
    host_http_status = 200;
    host_http_body = std::string(9000, 'x');
    assert(!fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));

    // A timezone or stale-RTC offset must not reject a fresh ticket from a
    // certificate-verified broker. Keep the same expiry bounds against Date.
    const auto server_now = saved_agent_http_date("Wed, 21 Oct 2026 00:49:51 GMT");
    assert(server_now && *server_now == 1792543791);
    assert(saved_agent_http_date("Tue, 01 Mar 2028 00:00:00 GMT") == std::nullopt); // wrong weekday
    assert(saved_agent_http_date("Tue, 29 Feb 2028 00:00:00 GMT"));
    for (const auto* bad : {"Wed, 29 Feb 2026 00:00:00 GMT", "Wed, 21 Oct 2026 24:00:00 GMT",
                            "Wed, 21 Oct 2026 00:00:00 JST", "Wed, 21 Oct 2026 00:00:00 GMT extra", "garbage"}) {
        assert(!saved_agent_http_date(bad));
    }
    for (const int skew : {-9 * 3600, 9 * 3600}) {
        const std::time_t remote_now = std::time(nullptr) + skew;
        char date[64]{};
        assert(std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", std::gmtime(&remote_now)) == 29);
        host_http_headers = {{"Date", date}};
        host_http_body = ticket_json("agent_test_only", remote_now + 300);
        assert(!parse_agent_ticket(host_http_body.data(), host_http_body.size(), std::time(nullptr)));
        assert(fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
        host_http_status = 302;
        assert(!fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
        host_http_status = 200;
    }
    host_http_headers = {{"dAtE", "Wed, 21 Oct 2026 00:49:51 GMT"}};
    host_http_body = ticket_json("agent_test_only", *server_now + 300);
    assert(fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
    host_http_body = ticket_json("agent_test_only", *server_now - 1);
    assert(!fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
    host_http_body = ticket_json("agent_test_only", *server_now + 3600);
    assert(!fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
    host_http_body = ticket_json("agent_test_only", *server_now + 300);
    host_http_headers.push_back({"Date", "Wed, 21 Oct 2026 00:49:51 GMT"});
    assert(!fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
    host_http_headers = {{"Date", "invalid"}};
    assert(!fetch_agent_ticket(kSavedAgentTokenUrl, std::string(43, 'd')));
    host_http_headers.clear();

    OpenAiRealtimeClient client("fake-ephemeral-token", RealtimeBackend::Xai);
    std::vector<ConversationEvent> events;
    client.set_event_callback([&](const auto& e) {events.push_back(e);});
    ConversationConfig cfg;
    cfg.agent_id = "agent_test_only";
    cfg.extra_headers = "Authorization: Bearer must-not-replace-token";
    assert(!client.start(cfg)); // A credential alone is not paid-test permission.
    bool permitted = true;
    cfg.session_permitted = [&] { return permitted; };
    assert(client.start(cfg));
    assert(host_socket->uri == "wss://api.x.ai/v1/realtime?agent_id=agent_test_only");
    assert(host_socket->headers.at("Authorization") == "Bearer fake-ephemeral-token");
    host_connected();
    assert(host_socket->sent.size() == 1);
    host_receive(R"({"type":"session.updated","session":{}})");
    assert(client.state() == ConversationState::Connecting && host_socket->sent.size() == 2);
    host_receive(confirmed);
    assert(client.state() == ConversationState::Listening);
    permitted = false;
    assert(!client.push_audio(std::array<std::int16_t, 960>{}));
    permitted = true;
    std::array<std::int16_t, 960> pcm{};
    pcm[0] = 0x1234; pcm[1] = -52;
    assert(client.push_audio(pcm));
    void* queued{};
    assert(xQueueReceive(host_last_queue, &queued, 0));
    auto* bytes = static_cast<std::uint8_t*>(queued);
    assert(*static_cast<std::uint16_t*>(queued) == 1920);
    assert(bytes[2] == 0x34 && bytes[3] == 0x12 && bytes[4] == 0xcc && bytes[5] == 0xff);
    heap_caps_free(queued);
    // speech_stopped must not cut off the saved-Agent input transport while
    // the server is still finalizing the turn and preparing its audio reply.
    host_receive(R"({"type":"input_audio_buffer.speech_stopped"})");
    assert(client.state() == ConversationState::Thinking);
    assert(client.push_audio(pcm));
    assert(xQueueReceive(host_last_queue, &queued, 0));
    assert(*static_cast<std::uint16_t*>(queued) == 1920);
    heap_caps_free(queued);
    host_receive(R"({"type":"input_audio_buffer.speech_started"})");
    std::array<std::int16_t, 1025> too_large{};
    assert(!client.push_audio(too_large));
    for (const char* name : {"web_search", "x_keyword_search", "x_semantic_search", "x_user_search", "x_thread_fetch"}) {
        const auto count = events.size();
        host_receive(std::string(R"({"type":"response.function_call_arguments.done","name":")") + name + R"(","call_id":"t","arguments":"{}"})");
        assert(events.size() == count);
    }
    host_receive(R"({"type":"response.created","response":{"id":"r1"}})");
    // VAD-only/blank events do not stop input; a recognized utterance does.
    host_receive(R"({"type":"conversation.item.input_audio_transcription.completed","transcript":" \t"})");
    assert(client.push_audio(pcm));
    assert(xQueueReceive(host_last_queue, &queued, 0));
    heap_caps_free(queued);
    host_receive(R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"test phrase"})");
    assert(client.push_audio(pcm));
    assert(!xQueueReceive(host_last_queue, &queued, 0));
    host_receive(R"({"type":"input_audio_buffer.speech_started"})");
    assert(client.push_audio(pcm));
    assert(!xQueueReceive(host_last_queue, &queued, 0)); // No reopening in this session.
    host_receive(R"({"type":"response.output_audio.delta","delta":"NBLM/w=="})");
    assert(events.back().audio && events.back().audio->size() == 2);
    assert(events.back().audio->at(0) == 0x1234 && events.back().audio->at(1) == -52);
    assert(events.back().emit_us == esp_timer_get_time());
    // Regression: a real 162809-byte server audio frame was dropped at 128 KiB.
    const auto large_audio = std::string(R"({"type":"response.output_audio.delta","delta":")") +
                             std::string(162760, 'A') + R"("})";
    assert(large_audio.size() > 131072);
    receive_fragmented(large_audio);
    assert(events.back().type == ConversationEventType::AssistantAudioChunk);
    assert(events.back().audio && events.back().audio->size() == 61035);
    assert(events.back().audio->front() == 0 && events.back().audio->back() == 0);
    auto error_count = [&] {
        return std::count_if(events.begin(), events.end(), [](const auto& e) { return e.type == ConversationEventType::Error; });
    };
    auto errors = error_count();
    receive_fragmented(std::string(1024 * 1024, 'x'));
    assert(error_count() == errors + 1); // One error per rejected frame, not per fragment.
    host_fail_heap_alloc_at_least = 512 * 1024;
    errors = error_count();
    receive_fragmented(std::string(300000, 'x'));
    assert(error_count() == errors + 1);
    assert(events.back().error == ConversationError::OutOfMemory);
    host_fail_heap_alloc_at_least = static_cast<std::size_t>(-1);
    host_receive(R"({"type":"response.output_audio.delta","delta":"NBLM/w=="})");
    assert(events.back().audio && events.back().audio->size() == 2); // Old buffer survives failed growth.
    host_receive(R"({"type":"response.done","response":{"status":"completed"}})");
    assert(client.state() == ConversationState::Listening);
    host_receive(R"({"type":"response.function_call_arguments.done","name":"x_search_arbitrary","call_id":"bad"})");
    assert(events.back().type == ConversationEventType::Error);
    client.stop();

    // An explicit same-name client declaration overrides native classification.
    assert(client.start(cfg)); host_connected(); host_receive(confirmed);
    host_receive(confirmed); // Saved-Agent readiness needs the session.update acknowledgement.
    assert(client.push_audio(pcm)); // Explicit new session resets the input gate.
    assert(xQueueReceive(host_last_queue, &queued, 0));
    heap_caps_free(queued);
    auto* root = cJSON_Parse(confirmed.c_str());
    auto* tools = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"session"), "tools");
    auto* custom = cJSON_CreateObject();
    cJSON_AddStringToObject(custom,"type","function"); cJSON_AddStringToObject(custom,"name","web_search");
    cJSON_AddItemToArray(tools,custom);
    auto* text = cJSON_PrintUnformatted(root);
    host_receive(text); cJSON_free(text); cJSON_Delete(root);
    host_receive(R"({"type":"response.function_call_arguments.done","name":"web_search","call_id":"custom"})");
    assert(events.back().type == ConversationEventType::Error);
    client.stop();
    assert(client.start(cfg)); host_connected(); host_receive(confirmed);
    host_receive(R"({"type":"session.updated","session":{"audio":{"output":{"format":{"type":"audio/pcm","rate":8000}}}}})");
    assert(client.state() == ConversationState::Error);
    client.stop();
}
