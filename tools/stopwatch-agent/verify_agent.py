#!/usr/bin/env python3
"""Local P1 probe for a saved xAI Agent. Never a device credential service.

Only audio output transport is configured. Agent model, instructions, voice,
tools and turn detection remain server-owned. No raw server events are logged.
"""

import argparse
import asyncio
import base64
import binascii
from collections import Counter
from datetime import datetime, timezone
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time
from urllib.parse import parse_qs, urlencode, urlsplit
import warnings
import wave


TOKEN_URL = "https://api.x.ai/v1/realtime/client_secrets"
WS_URL = "wss://api.x.ai/v1/realtime"
TOOL_VERSION = "0.3.0"
SAMPLE_RATE = 24000
MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * 180
PROMPTS = (
    "你好，请按照你已经配置好的身份，简短介绍一下自己。",
    "请记住：我今天给小机器人取的测试昵称是蓝莓。简短确认即可。",
    "What nickname did I just give the robot? Please answer in English.",
    "请用中文解释一下，为什么天空通常看起来是蓝色的？简短回答。",
    "Please explain the same idea in one short English sentence.",
    "回到中文：给我一个适合下班后做的五分钟放松建议。",
    "请根据你已启用的网页搜索工具，查找一条近期公开的航天新闻并说明来源；如果没有可用工具，请直接说明。",
    "请根据你已启用的 X 搜索工具，查找一条近期公开的航天动态并说明来源；如果没有可用工具，请直接说明。",
    "What was the robot's test nickname? Answer briefly in English.",
    "最后用中文向蓝莓说一句简短的鼓励话。",
)


class ProbeError(Exception):
    """A deliberately bounded error; do not include upstream exception bodies."""

    def __init__(self, stage, code):
        self.stage = stage
        self.code = code
        super().__init__(f"{stage}: {code}")


class Redactor:
    def __init__(self, *secrets):
        self.secrets = [s for s in secrets if s]

    def add(self, secret):
        if secret:
            self.secrets.append(secret)

    def text(self, value):
        for secret in sorted(self.secrets, key=len, reverse=True):
            value = value.replace(secret, "[REDACTED]")
        return value

    def clean(self, value):
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, list):
            return [self.clean(item) for item in value]
        if isinstance(value, dict):
            return {self.text(str(k)): self.clean(v) for k, v in value.items()}
        return value


def agent_url(agent_id):
    if not re.fullmatch(r"agent_[A-Za-z0-9_-]{3,160}", agent_id):
        raise ProbeError("input", "invalid_agent_id_copy_from_deployment")
    # Deliberately no model query and no generic fallback.
    return WS_URL + "?" + urlencode({"agent_id": agent_id})


def audio_config():
    return {"type": "session.update", "session": {"audio": {"output": {
        "format": {"type": "audio/pcm", "rate": SAMPLE_RATE},
        "transport": "json",
    }}}}


def config_evidence(session):
    """Hash only published config fields the server actually disclosed.

    Missing fields stay missing: an empty hash must not prove configuration
    equality. Fingerprints are evidence for comparison, never Agent validation.
    """
    result = {}
    for field in ("model", "voice", "instructions", "tools"):
        if field in session:
            raw = json.dumps(session[field], sort_keys=True, ensure_ascii=False)
            result[field] = hashlib.sha256(raw.encode("utf-8")).hexdigest()
    return result


def config_metadata(session, expected_agent_id=None):
    """Presence/emptiness is useful evidence without publishing configuration."""
    result = {}
    for field in ("model", "voice", "instructions", "tools"):
        if field not in session:
            state = "missing"
        elif session[field] is None:
            state = "null"
        elif isinstance(session[field], str):
            state = "populated" if session[field].strip() else "empty"
        elif isinstance(session[field], list):
            state = "populated" if session[field] else "empty"
        else:
            state = "other_type"
        result[field] = state
    result["agent_id_present"] = "agent_id" in session
    if "agent_id" in session and expected_agent_id:
        result["agent_id_matches_requested"] = session["agent_id"] == expected_agent_id
    return result


async def issue_token(api_key, *, client=None, now=None):
    import httpx

    if client is None:
        async with httpx.AsyncClient(timeout=30, follow_redirects=False) as owned:
            return await issue_token(api_key, client=owned, now=now)
    response = await client.post(
        TOKEN_URL,
        headers={"Authorization": "Bearer " + api_key},
        json={"expires_after": {"seconds": 300}},
    )
    if response.status_code != 200:
        raise ProbeError("token", f"http_{response.status_code}")
    try:
        data = response.json()
        token, expires_at = data["value"], data["expires_at"]
    except (ValueError, KeyError, TypeError):
        raise ProbeError("token", "unexpected_response_shape") from None
    if (not isinstance(token, str) or not token or "\n" in token or "\r" in token
            or isinstance(expires_at, bool) or not isinstance(expires_at, (int, float))):
        raise ProbeError("token", "unexpected_response_shape")
    if not expires_at > (time.time() if now is None else now) + 10:
        raise ProbeError("token", "expired_or_too_close_to_expiry")
    return token


async def send(ws, event):
    await ws.send(json.dumps(event, ensure_ascii=False))


async def receive(ws, deadline, counts):
    remaining = deadline - asyncio.get_running_loop().time()
    if remaining <= 0:
        raise ProbeError("events", "timeout")
    try:
        raw = await asyncio.wait_for(ws.recv(), remaining)
    except asyncio.TimeoutError:
        raise ProbeError("events", "timeout") from None
    if not isinstance(raw, str):
        raise ProbeError("events", "unexpected_binary_transport")
    try:
        event = json.loads(raw)
    except (TypeError, ValueError):
        raise ProbeError("events", "invalid_json") from None
    if not isinstance(event, dict) or not isinstance(event.get("type"), str):
        raise ProbeError("events", "missing_event_type")
    kind = event["type"]
    # Do not persist untrusted event names or raw error payloads.
    if not re.fullmatch(r"[a-zA-Z0-9_.]{1,100}", kind):
        raise ProbeError("events", "invalid_event_type")
    counts[kind] += 1
    if kind == "error":
        raise ProbeError("events", "server_error_check_console")
    return event



def safe_audio_shape(value):
    """Bounded schema information, excluding arbitrary strings and field names."""
    if isinstance(value, dict):
        allowed = ("type", "codec", "encoding", "format", "rate", "sample_rate",
                   "sample_rate_hz", "channels", "channel_count", "transport")
        # Only one level is needed to diagnose the known format containers.
        fields = {}
        for key in allowed:
            if key in value:
                child = value[key]
                fields[key] = {"kind": "object"} if isinstance(child, dict) else safe_audio_shape(child)
        return {"kind": "object", "fields": fields,
                "other_field_count": sum(key not in allowed for key in value)}
    if value is None:
        return {"kind": "null"}
    if isinstance(value, str):
        allowed = ("audio/pcm", "audio/pcmu", "audio/pcma", "audio/opus", "pcm16",
                   "g711_ulaw", "g711_alaw", "json", "binary")
        return {"kind": "string", "value": value if value in allowed else "other"}
    if type(value) is int:
        return {"kind": "integer", "value": value if value in (
            1, 2, 8000, 16000, 22050, 24000, 32000, 44100, 48000) else "other"}
    return {"kind": "other"}


def safe_audio_evidence(event):
    """Summarize *only* codec metadata; never log raw server events/secrets.

    This distinguishes 'not returned' from 'returned but wrong' without
    incorrectly treating a bare session.updated acknowledgment as PCM proof.
    """
    session = event.get("session")
    audio = session.get("audio") if isinstance(session, dict) else None
    output = audio.get("output") if isinstance(audio, dict) else None
    fmt = output.get("format") if isinstance(output, dict) else None
    kind = fmt.get("type") if isinstance(fmt, dict) else None
    rate = fmt.get("rate") if isinstance(fmt, dict) else None
    transport = output.get("transport") if isinstance(output, dict) else None
    return {
        "session_object": isinstance(session, dict),
        "audio_object": isinstance(audio, dict),
        "output_object": isinstance(output, dict),
        "format_object": isinstance(fmt, dict),
        "format_type": kind if kind in ("audio/pcm", "audio/pcmu", "audio/pcma", "audio/opus")
                       else ("missing" if kind is None else "other"),
        "sample_rate": rate if type(rate) is int and rate in (
            8000, 16000, 22050, 24000, 32000, 44100, 48000)
                       else ("missing" if rate is None else "other"),
        "transport": transport if transport in ("json", "binary")
                     else ("missing" if transport is None else "other"),
        "top_level_audio_present": "audio" in event,
        "legacy_output_audio_format_present":
            isinstance(session, dict) and "output_audio_format" in session,
        # Enumerate only known non-secret codec values. Never serialize arbitrary
        # server strings, which may contain unexpected private data.
        "legacy_output_audio_format": (
            session.get("output_audio_format")
            if isinstance(session, dict) and
               type(session.get("output_audio_format")) is str and
               session.get("output_audio_format") in
               ("pcm16", "g711_ulaw", "g711_alaw", "audio/pcm", "audio/pcmu", "audio/pcma")
            else ("missing" if not isinstance(session, dict) or
                  "output_audio_format" not in session else "other")
        ),
        "locations": audio_locations(event),
    }


def audio_locations(event):
    """Report known paths, including null/object distinctions V2 discarded."""
    found = {}
    paths = (
        ("session", "output_audio_format"),
        ("session", "output_audio_sample_rate"),
        ("session", "audio", "output"),
        ("session", "audio", "output", "format"),
        ("audio", "output"),
        ("audio", "output", "format"),
    )
    for path in paths:
        value = event
        for key in path:
            if not isinstance(value, dict) or key not in value:
                break
            value = value[key]
        else:
            found[".".join(path)] = safe_audio_shape(value)
    return found


def validate_audio(session, evidence, allow_legacy_pcm16):
    audio = session.get("audio")
    output = audio.get("output") if isinstance(audio, dict) else None
    output = output if isinstance(output, dict) else {}
    if output.get("transport", "json") != "json":
        raise ProbeError("session", "json_audio_not_confirmed")

    fmt = output.get("format")
    legacy = session.get("output_audio_format")
    # Prefer explicit nested metadata. A malformed/conflicting nested format
    # must never be masked by the legacy compatibility switch.
    nested = fmt is not None and fmt != {}
    selected = fmt if nested else legacy
    codec = selected.get("type") if isinstance(selected, dict) else selected
    evidence["audio_validation"] = "unconfirmed"
    evidence["sample_rate_explicitly_confirmed"] = False
    if codec not in ("audio/pcm", "pcm16"):
        evidence["audio_validation_detail"] = "nested_format_unrecognized" if nested else "legacy_format_missing_or_unrecognized"
        raise ProbeError("session", "pcm24k_not_confirmed")

    legacy_codec = legacy.get("type") if isinstance(legacy, dict) else legacy
    if nested and legacy_codec in ("g711_ulaw", "g711_alaw", "audio/pcmu", "audio/pcma", "audio/opus"):
        evidence["audio_validation_detail"] = "conflicting_codec_metadata"
        raise ProbeError("session", "pcm24k_not_confirmed")
    rates = []
    channels = []
    for container in (selected, output, legacy if nested else None):
        if isinstance(container, dict):
            rates.extend(container[key] for key in ("rate", "sample_rate", "sample_rate_hz") if key in container)
            channels.extend(container[key] for key in ("channels", "channel_count") if key in container)
    if "output_audio_sample_rate" in session:
        rates.append(session["output_audio_sample_rate"])
    if any(type(rate) is not int or rate != SAMPLE_RATE for rate in rates):
        evidence["audio_validation_detail"] = "rate_not_24000_or_conflicting"
        raise ProbeError("session", "pcm24k_not_confirmed")
    if any(type(channel) is not int or channel != 1 for channel in channels):
        evidence["audio_validation_detail"] = "mono_not_confirmed"
        raise ProbeError("session", "pcm24k_not_confirmed")
    if rates:
        evidence["audio_validation"] = "explicit_nested_pcm_24000" if nested else "explicit_legacy_pcm_24000"
        evidence["sample_rate_explicitly_confirmed"] = True
    elif not nested and codec == "pcm16" and allow_legacy_pcm16:
        evidence["audio_validation"] = "legacy_pcm16_rate_24000_assumed"
    else:
        evidence["audio_validation_detail"] = "rate_not_explicitly_returned"
        raise ProbeError("session", "pcm24k_not_confirmed")


async def configure(ws, counts, evidence, timeout, *, allow_legacy_pcm16=False,
                    diagnostic=False, expected_agent_id=None):
    deadline = asyncio.get_running_loop().time() + timeout
    while True:
        event = await receive(ws, deadline, counts)
        if event["type"] == "session.created":
            evidence["created_audio_evidence"] = safe_audio_evidence(event)
            session = event.get("session")
            if not isinstance(session, dict):
                raise ProbeError("session", "session_object_missing")
            evidence["created_config_fingerprints"] = config_evidence(session)
            evidence["created_config_metadata"] = config_metadata(session, expected_agent_id)
            break
    await send(ws, audio_config())
    while True:
        event = await receive(ws, deadline, counts)
        if event["type"] == "session.updated":
            evidence["updated_audio_evidence"] = safe_audio_evidence(event)
            session = event.get("session")
            if not isinstance(session, dict):
                raise ProbeError("session", "session_object_missing")
            evidence["session_update_received"] = True
            updated = config_evidence(session)
            evidence["updated_config_fingerprints"] = updated
            evidence["updated_config_metadata"] = config_metadata(session, expected_agent_id)
            created = evidence["created_config_fingerprints"]
            changed = sorted(field for field in created.keys() & updated.keys()
                             if created[field] != updated[field])
            evidence["changed_published_fields"] = changed
            try:
                validate_audio(session, evidence, allow_legacy_pcm16)
                evidence["audio_configured"] = True
            except ProbeError as error:
                evidence["audio_configured"] = False
                evidence["audio_error"] = classify_error(error)
                if not diagnostic:
                    raise
            if changed:
                error = ProbeError("session", "published_config_changed_after_audio_update")
                evidence["config_error"] = classify_error(error)
                if not diagnostic:
                    raise error
            if any(evidence[key].get("agent_id_matches_requested") is False
                   for key in ("created_config_metadata", "updated_config_metadata")):
                error = ProbeError("session", "returned_agent_id_mismatch")
                evidence["config_error"] = classify_error(error)
                if not diagnostic:
                    raise error
            return


async def collect_reply(ws, counts, timeout, *, cancel=False):
    deadline = asyncio.get_running_loop().time() + timeout
    started = asyncio.get_running_loop().time()
    pcm = bytearray()
    transcript = []
    first_audio_ms = None
    cancel_sent = False
    response_id = None
    while True:
        event = await receive(ws, deadline, counts)
        kind = event["type"]
        if kind == "response.created":
            response_id = event.get("response", {}).get("id")
            if not isinstance(response_id, str) or not response_id:
                raise ProbeError("response", "missing_response_id")
        if kind.startswith("response.") and response_id is None:
            # Late deltas can arrive before the next response.created as well.
            continue
        # Ignore deltas belonging to an older completed/cancelled response.
        if response_id and event.get("response_id", response_id) != response_id:
            continue
        if kind in ("response.output_audio.delta", "response.audio.delta"):
            try:
                audio = base64.b64decode(event["delta"], validate=True)
            except (KeyError, TypeError, ValueError, binascii.Error):
                raise ProbeError("audio", "invalid_base64") from None
            if first_audio_ms is None:
                first_audio_ms = round((asyncio.get_running_loop().time() - started) * 1000)
            if cancel:
                if not cancel_sent:
                    await send(ws, {"type": "response.cancel"})
                    cancel_sent = True
                # Protocol probe has no live speaker queue; discard probe audio.
            else:
                if len(pcm) + len(audio) > MAX_AUDIO_BYTES:
                    raise ProbeError("audio", "reply_exceeds_180_seconds")
                pcm.extend(audio)
        elif kind in ("response.output_audio_transcript.delta", "response.audio_transcript.delta",
                      "response.output_text.delta", "response.text.delta"):
            if isinstance(event.get("delta"), str):
                transcript.append(event["delta"])
        elif kind == "response.function_call_arguments.done":
            # Built-in Web/X Search runs server-side. External client functions
            # need real handlers; never fabricate a successful tool result.
            raise ProbeError("tools", "client_function_handler_required")
        elif kind == "response.done":
            response = event.get("response", {})
            if response_id and response.get("id", response_id) != response_id:
                continue
            status = response.get("status")
            if cancel:
                return {"cancel_sent": cancel_sent,
                        "cancel_acknowledged": cancel_sent and status == "cancelled",
                        "status": status if status in ("cancelled", "completed") else "other"}, b"", ""
            if status != "completed":
                raise ProbeError("response", "not_completed")
            if not pcm or len(pcm) % 2:
                raise ProbeError("audio", "missing_or_misaligned_pcm16")
            if not transcript:
                raise ProbeError("transcript", "missing_text_deltas")
            return {"completed": True, "audio_bytes": len(pcm),
                    "audio_seconds": round(len(pcm) / (SAMPLE_RATE * 2), 3),
                    "first_audio_ms": first_audio_ms}, bytes(pcm), "".join(transcript)


async def ask(ws, text):
    await send(ws, {"type": "conversation.item.create", "item": {
        "type": "message", "role": "user",
        "content": [{"type": "input_text", "text": text}],
    }})
    await send(ws, {"type": "response.create"})


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(SAMPLE_RATE)
        out.writeframes(pcm)


def classify_error(error):
    if isinstance(error, ProbeError):
        return {"stage": error.stage, "code": error.code}
    response = getattr(error, "response", None)
    status = getattr(response, "status_code", None)
    if isinstance(status, int):
        return {"stage": "websocket", "code": f"http_{status}"}
    # Exception messages/tracebacks may contain credentials, URLs or headers.
    return {"stage": "transport_or_local_io", "code": type(error).__name__}


async def probe_mode(mode, api_key, url, args, folder, redactor, result):
    from websockets.asyncio.client import connect

    counts = Counter()
    try:
        credential = api_key
        if mode == "ephemeral":
            credential = await issue_token(api_key)
            redactor.add(credential)
            result["token_issued"] = True
        async with connect(url, additional_headers={"Authorization": "Bearer " + credential},
                           open_timeout=30, max_size=2**22) as ws:
            result["connected"] = True
            diagnostic = getattr(args, "session_only", False)
            await configure(ws, counts, result, args.timeout,
                            allow_legacy_pcm16=getattr(args, "allow_legacy_pcm16", False),
                            diagnostic=diagnostic,
                            expected_agent_id=parse_qs(urlsplit(url).query).get("agent_id", [None])[0])
            if diagnostic:
                # An unrecognized format is a diagnostic result, not a reason
                # to discard the updated config evidence or generate audio.
                result["session_only_completed"] = True
                status = "已确认" if result.get("audio_configured") else "未确认（不会生成或播放音频）"
                print(f"[{mode}] 诊断采集完成；音频格式{status}")
                meta = result.get("updated_config_metadata", {})
                print(f"[{mode}] 服务端返回的人设状态: {meta.get('instructions', 'missing')}；工具状态: {meta.get('tools', 'missing')}")
                return
            for number, prompt in enumerate(PROMPTS[:args.rounds], 1):
                print(f"[{mode}] {number}/{args.rounds}: {prompt}")
                await ask(ws, prompt)
                turn, pcm, transcript = await collect_reply(ws, counts, args.timeout)
                filename = f"{mode}-{number:02d}.wav"
                write_wav(folder / filename, pcm)
                turn["audio_file"] = filename
                result["turns"].append(turn)
                print(redactor.text(transcript))
                if args.play:
                    if sys.platform == "win32":
                        import winsound
                        await asyncio.to_thread(winsound.PlaySound, str(folder / filename),
                                                winsound.SND_FILENAME)
                    else:
                        print("试听: " + filename)
                if args.cancel_probe and number == max(1, args.rounds - 1):
                    await ask(ws, "请从一数到一百，每个数字之间稍作停顿。")
                    result["cancel_probe"], _, _ = await collect_reply(
                        ws, counts, args.timeout, cancel=True)
                    result["cancel_probe"]["following_turn_completed"] = False
                elif "cancel_probe" in result:
                    result["cancel_probe"]["following_turn_completed"] = True
            result["protocol_ok"] = True
    except Exception as error:
        result["error"] = classify_error(error)
        print(f"[{mode}] 未通过: {result['error']['stage']} / {result['error']['code']}")
    finally:
        result["event_counts"] = dict(counts)


def compare_configs(results):
    if not all(mode in results for mode in ("key", "ephemeral")):
        return {"status": "not_run"}
    basis = "updated" if all("updated_config_fingerprints" in results[mode]
                             for mode in ("key", "ephemeral")) else "created"
    a = results["key"].get(basis + "_config_fingerprints", {})
    b = results["ephemeral"].get(basis + "_config_fingerprints", {})
    shared = sorted(a.keys() & b.keys())
    missing = sorted({"model", "voice", "instructions", "tools"} - set(shared))
    mismatch = [field for field in shared if a[field] != b[field]]
    empty = sorted(field for field in shared if a[field] == b[field] and
                   a[field] in (hashlib.sha256(b'""').hexdigest(), hashlib.sha256(b'[]').hexdigest()))
    status = "mismatch" if mismatch else ("partial" if missing else ("match_with_empty_fields" if empty else "match"))
    return {"status": status, "basis": basis,
            "compared_fields": shared, "unavailable_fields": missing,
            "mismatched_fields": mismatch, "empty_fields": empty,
            "agent_configuration_verified": False}


async def run(args, api_key, agent_id):
    url = agent_url(agent_id)
    redactor = Redactor(api_key, agent_id)
    folder = Path(args.output) / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    folder.mkdir(parents=True, exist_ok=False)
    session_only = getattr(args, "session_only", False)
    report = {"schema_version": 2, "tool_version": TOOL_VERSION,
              "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "rounds_requested_per_mode": 0 if session_only else args.rounds,
              "test_scope": "session_only" if session_only else "dialogue",
              "input_mode": "none" if session_only else "text",
              "requested_output_format": "PCM16 mono 24000 Hz",
              "output_format": "not_generated" if session_only else "see_each_result_audio_validation",
              "legacy_pcm16_opt_in": getattr(args, "allow_legacy_pcm16", False),
              "manual_review": {"persona": "pending", "voice": "pending",
                                "bilingual_and_memory": "pending", "web_x_tools": "pending"},
              "microphone_input": "not_tested", "natural_interruption": "not_tested",
              "device_firmware": "not_tested", "results": {}}
    modes = ("key", "ephemeral") if args.auth == "compare" else (args.auth,)
    try:
        for mode in modes:
            result = {"connected": False, "protocol_ok": False, "turns": []}
            report["results"][mode] = result
            await probe_mode(mode, api_key, url, args, folder, redactor, result)
    finally:
        report["config_comparison"] = compare_configs(report["results"])
        # Defense in depth: even server-provided event names are redacted.
        (folder / "report.json").write_text(
            json.dumps(redactor.clean(report), ensure_ascii=False, indent=2), encoding="utf-8")
        print("结果目录: " + str(folder.resolve()))
        print("report.json 不含对话正文。录音仅保存在本机，分享前请自行检查。")
    passed = all((r.get("session_only_completed") if session_only else r["protocol_ok"])
                 for r in report["results"].values())
    if not session_only:
        passed = passed and report["config_comparison"]["status"] != "mismatch"
    if session_only:
        print("仅采集会话诊断；退出码 0 表示采集完成，不代表音频或已发布 Agent 验收通过。")
    return 0 if passed else 1


def read_private(name, label):
    value = os.environ.get(name, "").strip()
    if not value:
        if not sys.stdin.isatty():
            raise ProbeError("input", "interactive_terminal_required")
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            try:
                value = getpass.getpass(label).strip()
            except getpass.GetPassWarning:
                raise ProbeError("input", "hidden_input_unavailable") from None
    if not value or "\n" in value or "\r" in value:
        raise ProbeError("input", "empty_or_multiline_value")
    return value


def main():
    parser = argparse.ArgumentParser(description="StopWatch 已发布 Agent 的本机 P1 对照测试")
    parser.add_argument("--auth", choices=("compare", "key", "ephemeral"), default="compare")
    parser.add_argument("--rounds", type=int, choices=range(1, 11), default=1)
    parser.add_argument("--session-only", action="store_true",
                        help="只采集连接与会话结构；格式未确认也保存证据，不发起对话")
    parser.add_argument("--allow-legacy-pcm16", action="store_true",
                        help="兼容只返回旧版 pcm16 编码字段的服务端；24kHz 依据默认值推定，非明确确认")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--play", action="store_true", help="Windows 每轮自动播放回复")
    parser.add_argument("--cancel-probe", action="store_true", help="额外测试取消协议，不是自然打断")
    parser.add_argument("--output", default=str(Path(__file__).parent / "results"))
    args = parser.parse_args()
    if not 5 <= args.timeout <= 600:
        parser.error("timeout must be 5..600 seconds")
    print(f"StopWatch Agent 验证工具 {TOOL_VERSION}；不修改设备。")
    if args.session_only:
        print("会话诊断模式：只采集两种鉴权的会话结构，不生成语音回复；连接仍可能产生 API 用量。" if args.auth == "compare" else
              "会话诊断模式：不生成语音回复；连接仍可能产生 API 用量。")
    else:
        modes = "两种鉴权" if args.auth == "compare" else args.auth
        print(f"对话测试：{modes}，每种 {args.rounds} 轮，会使用 xAI API 额度。Ctrl+C 可停止。")
    if args.allow_legacy_pcm16:
        print("兼容模式只对已识别的旧版 pcm16 生效；缺失采样率时推定 24kHz，不是实测。")
    try:
        api_key = read_private("XAI_API_KEY", "粘贴 xAI API Key（隐藏输入），回车: ")
        agent_id = read_private("XAI_AGENT_ID", "粘贴 Deployment 中的 agent_id（隐藏输入），回车: ")
        return asyncio.run(run(args, api_key, agent_id))
    except KeyboardInterrupt:
        print("已停止；已完成的结果保存在本机。")
        return 130
    except Exception as error:
        safe = classify_error(error)
        print(f"未开始或意外停止: {safe['stage']} / {safe['code']}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
