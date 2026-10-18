import argparse
import asyncio
import base64
from collections import Counter
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
import wave

import httpx

import verify_agent as probe


CONFIG = {"model": "saved-model", "voice": "saved-voice",
          "instructions": "saved-persona", "tools": [{"type": "web_search"}]}


class Socket:
    def __init__(self, events=None):
        self.events = list(events or [{"type": "session.created", "session": CONFIG}])
        self.sent = []
        self.number = 0
        self.current_session = CONFIG
        self.configured = False

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        pass

    async def recv(self):
        if not self.events:
            await asyncio.Future()
        event = self.events.pop(0)
        if event["type"] == "session.updated":
            self.current_session = event["session"]
            self.configured = True
        return json.dumps(event)

    async def send(self, raw):
        event = json.loads(raw)
        self.sent.append(event)
        if event["type"] == "session.update":
            if any(e["type"] == "session.updated" for e in self.events):
                return
            self.events.append({"type": "session.updated", "session": {
                **self.current_session,
                **({} if self.configured else probe.audio_config()["session"])}})
        elif event["type"] == "response.create":
            self.number += 1
            rid = f"reply-{self.number}"
            self.events.extend([
                {"type": "response.created", "response": {"id": rid}},
                {"type": "response.output_audio.delta", "response_id": rid,
                 "delta": base64.b64encode(b"\0\0" * 240).decode()},
                {"type": "response.output_audio_transcript.delta", "response_id": rid,
                 "delta": "reply"},
                {"type": "response.done", "response": {"id": rid, "status": "completed"}},
            ])


class AgentProbeTests(unittest.IsolatedAsyncioTestCase):
    async def test_token_request_schema_and_auth_are_server_side(self):
        def handler(request):
            self.assertEqual(str(request.url), probe.TOKEN_URL)
            self.assertEqual(request.headers["Authorization"], "Bearer private-key")
            self.assertEqual(json.loads(request.content), {"expires_after": {"seconds": 300}})
            return httpx.Response(200, json={"value": "short-token", "expires_at": 400})
        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            self.assertEqual(await probe.issue_token("private-key", client=client, now=100), "short-token")

    async def test_invalid_token_responses_never_expose_upstream_body(self):
        cases = [(401, {"message": "private-key"}), (302, {}),
                 (200, {"value": "short-token", "expires_at": 105}),
                 (200, {"value": "bad\nheader", "expires_at": 400}),
                 (200, {"client_secret": {"value": "short-token"}})]
        for status, payload in cases:
            with self.subTest(status=status, payload=payload):
                transport = httpx.MockTransport(lambda req: httpx.Response(status, json=payload))
                async with httpx.AsyncClient(transport=transport) as client:
                    with self.assertRaises(probe.ProbeError) as caught:
                        await probe.issue_token("private-key", client=client, now=100)
                    self.assertNotIn("private-key", str(caught.exception))
                    self.assertNotIn("short-token", str(caught.exception))

    async def test_audio_update_preserves_published_agent_fields(self):
        socket, evidence = Socket(), {}
        await probe.configure(socket, Counter(), evidence, 5)
        self.assertEqual(list(socket.sent[0]["session"]), ["audio"])
        self.assertEqual(evidence["changed_published_fields"], [])
        self.assertEqual(probe.agent_url("agent_example"),
                         "wss://api.x.ai/v1/realtime?agent_id=agent_example")
        self.assertNotIn("model=", probe.agent_url("agent_example"))
        with self.assertRaises(probe.ProbeError):
            probe.agent_url("agent_example&model=override")

    async def test_audio_update_rejects_config_change_or_unknown_codec(self):
        for session in ({**CONFIG, "voice": "different", **probe.audio_config()["session"]}, CONFIG):
            socket = Socket([{"type": "session.created", "session": CONFIG},
                             {"type": "session.updated", "session": {
                                 **CONFIG, **probe.audio_config()["session"]}},
                             {"type": "session.updated", "session": session}])
            with self.assertRaises(probe.ProbeError):
                await probe.configure(socket, Counter(), {}, 5)


    async def test_session_update_missing_audio_is_rejected_and_reported_safely(self):
        secret = "SECRET_DO_NOT_LOG"
        socket = Socket([{"type": "session.created", "session": CONFIG},
                         {"type": "session.updated", "session": {
                             **CONFIG, "instructions": secret}}])
        evidence = {}
        with self.assertRaises(probe.ProbeError) as caught:
            await probe.configure(socket, Counter(), evidence, 5)
            self.assertEqual(caught.exception.code, "pcm24k_not_confirmed")
        self.assertEqual(evidence["updated_audio_evidence"]["format_type"], "missing")
        self.assertFalse(evidence["updated_audio_evidence"]["output_object"])
        self.assertNotIn(secret, json.dumps(evidence))

    async def test_legacy_pcm16_recorded_but_rejected_by_default(self):
        session = {**CONFIG, "output_audio_format": "pcm16",
                   "audio": {"output": {"transport": "json"}}}
        socket = Socket([{"type": "session.created", "session": CONFIG},
                         {"type": "session.updated", "session": session}])
        evidence = {}
        with self.assertRaises(probe.ProbeError) as caught:
            await probe.configure(socket, Counter(), evidence, 5)
        self.assertEqual(caught.exception.code, "pcm24k_not_confirmed")
        self.assertEqual(evidence["updated_audio_evidence"]["legacy_output_audio_format"], "pcm16")

    async def test_opt_in_legacy_pcm16_marks_assumed_rate(self):
        session = {**CONFIG, "output_audio_format": "pcm16",
                   "audio": {"output": {"transport": "json"}}}
        socket = Socket([{"type": "session.created", "session": CONFIG},
                         {"type": "session.updated", "session": session}])
        evidence = {}
        await probe.configure(socket, Counter(), evidence, 5, allow_legacy_pcm16=True)
        self.assertEqual(evidence["audio_validation"], "legacy_pcm16_rate_24000_assumed")
        self.assertIs(evidence["sample_rate_explicitly_confirmed"], False)
        self.assertEqual(evidence["changed_published_fields"], [])

    async def test_legacy_non_pcm16_and_conflicting_nested_format_are_rejected(self):
        for session in (
            {**CONFIG, "output_audio_format": "g711_ulaw",
             "audio": {"output": {"transport": "json"}}},
            {**CONFIG, "output_audio_format": "pcm16",
             "audio": {"output": {"transport": "json",
                                   "format": {"type": "audio/pcm", "rate": 16000}}}},
            {**CONFIG, "output_audio_format": "pcm16",
             "audio": {"output": {"transport": "binary"}}},
        ):
            with self.subTest(session=session):
                socket = Socket([{"type": "session.created", "session": CONFIG},
                                 {"type": "session.updated", "session": session}])
                with self.assertRaises(probe.ProbeError):
                    await probe.configure(socket, Counter(), {}, 5, allow_legacy_pcm16=True)

    def test_legacy_metadata_never_logs_unknown_strings(self):
        for suspicious in ("my-api-key", {"token": "secret"}, 123, None):
            evidence = probe.safe_audio_evidence({"session": {
                "output_audio_format": suspicious}})
            self.assertEqual(evidence["legacy_output_audio_format"], "other")
            self.assertNotIn("secret", json.dumps(evidence))

    def test_audio_shapes_distinguish_null_string_and_object_without_leaks(self):
        for value, kind in ((None, "null"), ("pcm16", "string"),
                            ({"type": "audio/pcm", "rate": 24000}, "object")):
            evidence = probe.safe_audio_evidence({"session": {"output_audio_format": value}})
            self.assertEqual(evidence["locations"]["session.output_audio_format"]["kind"], kind)
        unsafe = {"session": {"output_audio_format": {"type": "SECRET_TYPE", "api_key": "SECRET_KEY"},
                              "audio": {"output": {"format": {"type": "SECRET_TYPE", "rate": 24000}}}}}
        rendered = json.dumps(probe.safe_audio_evidence(unsafe))
        self.assertNotIn("SECRET", rendered)
        self.assertNotIn("api_key", rendered)

    async def test_explicit_legacy_object_is_parsed_without_assuming_rate(self):
        for fmt in ({"type": "audio/pcm", "rate": 24000},
                    {"type": "pcm16", "sample_rate": 24000, "channels": 1}):
            session = {**CONFIG, "output_audio_format": fmt,
                       "audio": {"output": {"transport": "json"}}}
            socket = Socket([{"type": "session.created", "session": CONFIG},
                             {"type": "session.updated", "session": session}])
            evidence = {}
            await probe.configure(socket, Counter(), evidence, 5)
            self.assertEqual(evidence["audio_validation"], "explicit_legacy_pcm_24000")
            self.assertTrue(evidence["sample_rate_explicitly_confirmed"])

    async def test_compatibility_never_masks_conflicting_rate_codec_or_channels(self):
        cases = (
            {"output_audio_format": "pcm16", "output_audio_sample_rate": 16000},
            {"output_audio_format": {"type": "pcm16", "rate": 24000, "sample_rate": 16000}},
            {"output_audio_format": {"type": "pcm16", "rate": 24000, "channels": 2}},
            {"output_audio_format": {"type": "audio/pcmu", "rate": 24000},
             "audio": {"output": {"format": {"type": "audio/pcm", "rate": 24000}}}},
            {"output_audio_format": "pcm16", "audio": {"output": {"format": "unrecognized"}}},
        )
        for extra in cases:
            with self.subTest(extra=extra):
                evidence = {}
                with self.assertRaises(probe.ProbeError):
                    probe.validate_audio({**CONFIG, **extra}, evidence, True)

    async def test_diagnostic_retains_config_when_format_missing_or_null(self):
        # User's real report established a legacy field was present, but did
        # not capture its value. Exercise possible shapes without inventing it.
        for legacy in (None, "unrecognized", {"unexpected": "SECRET"}):
            session = {**CONFIG, "instructions": "", "tools": [],
                       "output_audio_format": legacy, "audio": {"output": {"transport": "json"}}}
            socket = Socket([{"type": "session.created", "session": CONFIG},
                             {"type": "session.updated", "session": session}])
            evidence = {}
            await probe.configure(socket, Counter(), evidence, 5, diagnostic=True)
            self.assertFalse(evidence["audio_configured"])
            self.assertTrue(evidence["session_update_received"])
            self.assertEqual(evidence["updated_config_metadata"]["instructions"], "empty")
            self.assertEqual(evidence["updated_config_metadata"]["tools"], "empty")
            self.assertEqual(evidence["initial_changed_published_fields"], ["instructions", "tools"])
            self.assertEqual(evidence["changed_published_fields"], [])
            self.assertNotIn("SECRET", json.dumps(evidence))

    def test_empty_config_fingerprints_are_not_reported_as_full_match(self):
        empty = {"instructions": "", "tools": [], "voice": "voice", "model": "model"}
        results = {mode: {"updated_config_fingerprints": probe.config_evidence(empty)}
                   for mode in ("key", "ephemeral")}
        comparison = probe.compare_configs(results)
        self.assertEqual(comparison["status"], "match_with_empty_fields")
        self.assertEqual(comparison["basis"], "updated")
        self.assertEqual(comparison["empty_fields"], ["instructions", "tools"])
        self.assertFalse(comparison["agent_configuration_verified"])

    def test_agent_id_metadata_compares_without_storing_identifier(self):
        result = probe.config_metadata({**CONFIG, "agent_id": "agent_private"}, "agent_private")
        self.assertTrue(result["agent_id_matches_requested"])
        self.assertNotIn("agent_private", json.dumps(result))

    async def test_returned_different_agent_id_blocks_dialogue(self):
        session = {**CONFIG, "agent_id": "agent_different", **probe.audio_config()["session"]}
        socket = Socket([{"type": "session.created", "session": CONFIG},
                         {"type": "session.updated", "session": session}])
        with self.assertRaises(probe.ProbeError) as caught:
            await probe.configure(socket, Counter(), {}, 5, expected_agent_id="agent_requested")
        self.assertEqual(caught.exception.code, "returned_agent_id_mismatch")

    async def test_initial_agent_population_is_not_confused_with_later_changes(self):
        created = {**CONFIG, "instructions": "", "tools": [], "voice": "initial-voice"}
        saved = {**CONFIG, **probe.audio_config()["session"]}
        del saved["voice"]
        saved["audio"]["output"]["voice"] = CONFIG["voice"]
        socket = Socket([{"type": "session.created", "session": created},
                         {"type": "session.updated", "session": saved}])
        evidence = {}
        await probe.configure(socket, Counter(), evidence, 5)
        self.assertEqual(evidence["initial_changed_published_fields"], ["instructions", "tools", "voice"])
        self.assertEqual(evidence["changed_published_fields"], [])
        self.assertNotIn("config_error", evidence)
        self.assertEqual(evidence["confirmed_config_metadata"]["voice"], "populated")
        self.assertEqual(evidence["confirmed_config_metadata"]["voice_location"], "session.audio.output.voice")
        self.assertEqual(len(socket.sent), 2)
        self.assertTrue(all(list(event["session"]) == ["audio"] for event in socket.sent))

    async def test_unknown_codec_does_not_falsely_report_initial_population_as_overwrite(self):
        created = {**CONFIG, "instructions": "", "tools": []}
        saved = {**CONFIG, "output_audio_format": "vendor-pcm-variant",
                 "audio": {"output": {"transport": "json", "voice": CONFIG["voice"]}}}
        del saved["voice"]
        socket = Socket([{"type": "session.created", "session": created},
                         {"type": "session.updated", "session": saved}])
        evidence = {}
        await probe.configure(socket, Counter(), evidence, 5, diagnostic=True)
        self.assertFalse(evidence["audio_configured"])
        self.assertNotIn("config_error", evidence)
        self.assertEqual(evidence["confirmed_audio_evidence"]["legacy_output_audio_format_label"],
                         "vendor-pcm-variant")

    def test_codec_names_are_visible_but_key_like_values_are_not(self):
        for value in ("PCM16", "pcm_s16le", "audio/L16;rate=24000", "pcm_24000"):
            self.assertEqual(probe.codec_label(value), value)
        self.assertEqual(probe.codec_label(""), "empty")
        self.assertEqual(probe.codec_label("  "), "whitespace_only")
        for value in ("my-api-key", "Bearer xyz", "xai-private", "sk-private", "A" * 50,
                      "line\nbreak", "\x1b[31m"):
            self.assertEqual(probe.codec_label(value), "redacted_unrecognized_value")

    def test_pcm16_case_normalization_does_not_bypass_rate_confirmation(self):
        session = {"output_audio_format": "PCM16"}
        with self.assertRaises(probe.ProbeError):
            probe.validate_audio(session, {}, False)
        evidence = {}
        probe.validate_audio(session, evidence, True)
        self.assertFalse(evidence["sample_rate_explicitly_confirmed"])

    async def test_diagnostic_succeeds_at_collection_without_claiming_audio_success(self):
        sockets = []
        def connect(url, **kwargs):
            # Reconstruct only the known shape; the real legacy value remains
            # unknown until a new report is collected.
            session = {**CONFIG, "instructions": "", "tools": [],
                       "output_audio_format": None, "audio": {"output": {"transport": "json"}}}
            socket = Socket([{"type": "session.created", "session": session},
                             {"type": "session.updated", "session": session}])
            sockets.append(socket)
            return socket
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(auth="compare", rounds=10, timeout=5, output=directory,
                                      session_only=True, play=True, cancel_probe=True)
            with patch("websockets.asyncio.client.connect", connect), patch.object(
                    probe, "issue_token", AsyncMock(return_value="short-token")), patch("builtins.print"):
                self.assertEqual(await probe.run(args, "private-key", "agent_example"), 0)
            report = json.loads(next(Path(directory).rglob("report.json")).read_text(encoding="utf-8"))
            self.assertEqual(report["tool_version"], "0.3.1")
            self.assertEqual(report["output_format"], "not_generated")
            self.assertFalse(list(Path(directory).rglob("*.wav")))
            for mode in ("key", "ephemeral"):
                result = report["results"][mode]
                self.assertTrue(result["session_only_completed"])
                self.assertFalse(result["protocol_ok"])
                self.assertFalse(result["audio_configured"])
                self.assertIn("updated_config_fingerprints", result)
                self.assertEqual(result["audio_error"]["code"], "pcm24k_not_confirmed")
            self.assertFalse(any(event["type"] == "response.create"
                                 for socket in sockets for event in socket.sent))

    async def test_session_only_skips_response_create(self):
        sockets = []
        def connect(url, **kwargs):
            socket = Socket()
            sockets.append(socket)
            return socket
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(auth="key", rounds=10, timeout=5, output=directory,
                                      session_only=True, play=False, cancel_probe=False)
            with patch("websockets.asyncio.client.connect", connect), patch("builtins.print"):
                self.assertEqual(await probe.run(args, "private-key", "agent_example"), 0)
            self.assertFalse(any(event["type"] == "response.create" for event in sockets[0].sent))
            report = json.loads(next(Path(directory).rglob("report.json")).read_text(encoding="utf-8"))
            self.assertEqual(report["rounds_requested_per_mode"], 0)
            self.assertTrue(report["results"]["key"]["session_only_completed"])
            self.assertFalse(report["results"]["key"]["protocol_ok"])

    async def test_late_audio_cannot_pollute_next_reply(self):
        socket = Socket([
            {"type": "response.output_audio.delta", "response_id": "old", "delta": "%%%"},
            {"type": "response.done", "response": {"id": "old", "status": "cancelled"}},
            {"type": "response.created", "response": {"id": "new"}},
            {"type": "response.output_audio.delta", "response_id": "old", "delta": "%%%"},
            {"type": "response.output_audio.delta", "response_id": "new", "delta": "AAA="},
            {"type": "response.output_audio_transcript.delta", "response_id": "new", "delta": "ok"},
            {"type": "response.done", "response": {"id": "new", "status": "completed"}},
        ])
        turn, pcm, text = await probe.collect_reply(socket, Counter(), 5)
        self.assertEqual((pcm, text, turn["audio_bytes"]), (b"\0\0", "ok", 2))

    async def test_cancel_discards_audio_and_records_real_ack(self):
        socket = Socket([
            {"type": "response.created", "response": {"id": "r"}},
            {"type": "response.output_audio.delta", "delta": "AAA="},
            {"type": "response.output_audio.delta", "delta": "AAA="},
            {"type": "response.done", "response": {"id": "r", "status": "cancelled"}},
        ])
        result, pcm, _ = await probe.collect_reply(socket, Counter(), 5, cancel=True)
        self.assertTrue(result["cancel_acknowledged"])
        self.assertEqual(socket.sent, [{"type": "response.cancel"}])
        self.assertEqual(pcm, b"")

    async def test_server_errors_bad_audio_and_client_tools_are_failures(self):
        for event in ({"type": "error", "error": {"message": "private-key"}},
                      {"type": "response.output_audio.delta", "delta": "%%%"},
                      {"type": "response.function_call_arguments.done"}):
            with self.subTest(event=event["type"]):
                with self.assertRaises(probe.ProbeError) as caught:
                    await probe.collect_reply(Socket([
                        {"type": "response.created", "response": {"id": "r"}}, event]), Counter(), 5)
                self.assertNotIn("private-key", str(caught.exception))

    async def test_timeout_is_bounded(self):
        socket = Socket()
        socket.events.clear()
        with self.assertRaises(probe.ProbeError):
            await probe.receive(socket, asyncio.get_running_loop().time() + 0.01, Counter())

    async def test_ten_turn_comparison_uses_same_agent_and_different_credentials(self):
        sockets, connections = [], []
        def connect(url, **kwargs):
            connections.append((url, kwargs))
            socket = Socket()
            sockets.append(socket)
            return socket
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(auth="compare", rounds=10, timeout=5, output=directory,
                                      play=False, cancel_probe=False)
            with patch("websockets.asyncio.client.connect", connect), patch.object(
                    probe, "issue_token", AsyncMock(return_value="short-token")), patch("builtins.print"):
                self.assertEqual(await probe.run(args, "private-key", "agent_example"), 0)
            self.assertEqual(connections[0][0], connections[1][0])
            self.assertEqual([c[1]["additional_headers"]["Authorization"] for c in connections],
                             ["Bearer private-key", "Bearer short-token"])
            report_path = next(Path(directory).rglob("report.json"))
            text = report_path.read_text(encoding="utf-8")
            for secret in ("private-key", "short-token", "agent_example", "saved-persona"):
                self.assertNotIn(secret, text)
            report = json.loads(text)
            self.assertEqual(report["config_comparison"]["status"], "match")
            for mode in ("key", "ephemeral"):
                self.assertEqual(len(report["results"][mode]["turns"]), 10)
            self.assertEqual(report["manual_review"]["voice"], "pending")
            with wave.open(str(next(Path(directory).rglob("*.wav")))) as audio:
                self.assertEqual((audio.getnchannels(), audio.getsampwidth(), audio.getframerate()),
                                 (1, 2, 24000))

    async def test_token_failure_does_not_fall_back_to_key(self):
        args = argparse.Namespace(timeout=5, rounds=1, play=False, cancel_probe=False)
        result = {"protocol_ok": False, "turns": []}
        with patch("websockets.asyncio.client.connect") as connect, patch.object(
                probe, "issue_token", AsyncMock(side_effect=probe.ProbeError("token", "http_401"))), \
                patch("builtins.print"):
            await probe.probe_mode("ephemeral", "key", "wss://unused", args, Path("."),
                                   probe.Redactor("key"), result)
        connect.assert_not_called()
        self.assertFalse(result["protocol_ok"])
        self.assertEqual(result["error"], {"stage": "token", "code": "http_401"})

    def test_hidden_input_fails_closed_instead_of_echoing(self):
        with patch.dict("os.environ", {}, clear=True), patch("sys.stdin.isatty", return_value=True), \
                patch("getpass.getpass", side_effect=probe.getpass.GetPassWarning()):
            with self.assertRaises(probe.ProbeError) as caught:
                probe.read_private("XAI_API_KEY", "Key: ")
        self.assertEqual(caught.exception.code, "hidden_input_unavailable")

    def test_partial_fingerprints_and_redaction_never_create_false_equality(self):
        self.assertEqual(probe.compare_configs({"key": {}, "ephemeral": {}})["status"], "partial")
        self.assertEqual(probe.compare_configs({"key": {"created_config_fingerprints": {"voice": "a"}},
                                                "ephemeral": {"created_config_fingerprints": {"voice": "b"}}})
                         ["status"], "mismatch")
        redactor = probe.Redactor("long-secret", "agent_example")
        redactor.add("short-secret")
        self.assertEqual(redactor.clean({"x": ["long-secret agent_example short-secret"]}),
                         {"x": ["[REDACTED] [REDACTED] [REDACTED]"]})
        self.assertEqual(probe.classify_error(RuntimeError("Authorization: Bearer long-secret")),
                         {"stage": "transport_or_local_io", "code": "RuntimeError"})


if __name__ == "__main__":
    unittest.main()
