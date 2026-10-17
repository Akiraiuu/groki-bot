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
from urllib.parse import urlencode
import warnings
import wave


TOKEN_URL = "https://api.x.ai/v1/realtime/client_secrets"
WS_URL = "wss://api.x.ai/v1/realtime"
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


async def configure(ws, counts, evidence, timeout):
    deadline = asyncio.get_running_loop().time() + timeout
    while True:
        event = await receive(ws, deadline, counts)
        if event["type"] == "session.created":
            evidence["created_config_fingerprints"] = config_evidence(event.get("session", {}))
            break
    await send(ws, audio_config())
    while True:
        event = await receive(ws, deadline, counts)
        if event["type"] == "session.updated":
            session = event.get("session", {})
            output = session.get("audio", {}).get("output", {})
            fmt = output.get("format", {})
            # Never guess a sample rate and write misleading WAVs.
            if fmt.get("type") != "audio/pcm" or fmt.get("rate") != SAMPLE_RATE:
                raise ProbeError("session", "pcm24k_not_confirmed")
            if output.get("transport", "json") != "json":
                raise ProbeError("session", "json_audio_not_confirmed")
            updated = config_evidence(session)
            evidence["updated_config_fingerprints"] = updated
            created = evidence["created_config_fingerprints"]
            changed = [field for field in created.keys() & updated.keys()
                       if created[field] != updated[field]]
            evidence["changed_published_fields"] = changed
            if changed:
                raise ProbeError("session", "published_config_changed_after_audio_update")
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
            await configure(ws, counts, result, args.timeout)
            result["audio_configured"] = True
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
    a = results["key"].get("created_config_fingerprints", {})
    b = results["ephemeral"].get("created_config_fingerprints", {})
    shared = sorted(a.keys() & b.keys())
    missing = sorted({"model", "voice", "instructions", "tools"} - set(shared))
    mismatch = [field for field in shared if a[field] != b[field]]
    return {"status": "mismatch" if mismatch else ("partial" if missing else "match"),
            "compared_fields": shared, "unavailable_fields": missing,
            "mismatched_fields": mismatch}


async def run(args, api_key, agent_id):
    url = agent_url(agent_id)
    redactor = Redactor(api_key, agent_id)
    folder = Path(args.output) / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    folder.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 1, "rounds_requested_per_mode": args.rounds,
              "input_mode": "text", "output_format": "PCM16 mono 24000 Hz",
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
    return 0 if (all(r["protocol_ok"] for r in report["results"].values()) and
                 report["config_comparison"]["status"] != "mismatch") else 1


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
    parser.add_argument("--rounds", type=int, choices=range(1, 11), default=10)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--play", action="store_true", help="Windows 每轮自动播放回复")
    parser.add_argument("--cancel-probe", action="store_true", help="额外测试取消协议，不是自然打断")
    parser.add_argument("--output", default=str(Path(__file__).parent / "results"))
    args = parser.parse_args()
    if not 5 <= args.timeout <= 600:
        parser.error("timeout must be 5..600 seconds")
    print("仅在本机验证已发布 Agent，不修改设备。")
    print("默认对照两种鉴权，各 10 轮，会使用 xAI API 额度。Ctrl+C 可停止。")
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
