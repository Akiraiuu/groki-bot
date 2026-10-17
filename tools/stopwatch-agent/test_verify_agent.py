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

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        pass

    async def recv(self):
        if not self.events:
            await asyncio.Future()
        return json.dumps(self.events.pop(0))

    async def send(self, raw):
        event = json.loads(raw)
        self.sent.append(event)
        if event["type"] == "session.update":
            self.events.append({"type": "session.updated", "session": {
                **CONFIG, **probe.audio_config()["session"]}})
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
                             {"type": "session.updated", "session": session}])
            with self.assertRaises(probe.ProbeError):
                await probe.configure(socket, Counter(), {}, 5)

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
