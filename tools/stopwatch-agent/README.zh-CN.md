# StopWatch AI Assistant：电脑端诊断工具 v0.3.0

目标为完整沿用已发布的 **StopWatch AI Assistant**，最终让设备使用短期令牌直连 xAI。本工具只在电脑上诊断接口，不是固件或云端令牌服务。

## 这次只做一件事

1. 将**完整新版 ZIP**解压到新文件夹，保留旧文件和 results。不要继续在不同聊天提供的脚本之间反复覆盖。
2. 双击 `run-windows.cmd`。它会优先使用此文件夹已有的 `.venv`，否则尝试 `py`、`python` 创建环境，需要 Python 3.11 或更新版。已安装 Python 的用户不一定需要安装 `py` 启动器。
3. 确认窗口显示 **`StopWatch Agent 验证工具 0.3.0`**。依次粘贴自己的 xAI API Key、Deployment 的完整 `agent_id`，每次按回车。隐藏输入不显示字符或星号；密钥不发到聊天。
4. 等待两种鉴权完成诊断，打开 `results` 中**本次最新目录**，反馈 `report.json`。不要拿上一版本的报告替代。

双击默认运行的是 `--session-only --auth compare`：只建会话、发送一次音频输出设置并采集返回结构，不发送测试问题，不请求播放回复。建立连接仍可能产生 API 用量。**退出码 0 只表示诊断采集完成，报告的 `protocol_ok=false` 在这种模式下是正常的，不等于对话失败。**

尚未安装 Python：[Windows 下载页](https://www.python.org/downloads/windows/)。首次运行会安装 `requirements.txt` 中的两个直接依赖。

## 为什么之前一直报 pcm24k_not_confirmed

这是**本机脚本的校验错误码**。早期脚本只认 `session.audio.output.format.type/rate`；另一聊天的 V2 虽能在显式开关下接受旧式字符串 `pcm16`，但不能因此接受未知值、对象、null 或相互矛盾的字段。

用户提供的两份真实报告已经证实：Key 和短期令牌都曾连接成功，并收到 `session.created` / `session.updated`；更新响应有旧字段 `output_audio_format`，没有嵌套 format。**旧字段的实际值没有被记录；归档中也没有最新兼容模式失败后的完整报告，所以暂不能认定其根因就是某一种格式。**

v0.3.0 合并已审阅的 V2 修改，并补充：

- 区分缺失、null、字符串、对象；只输出白名单格式元数据及已知路径，不保存原始事件。
- 对明确携带 PCM 编码和 24000 Hz 的旧式对象结构进行确认；矛盾编码、不同采样率、立体声、binary 输出仍停止语音测试。
- 音频未确认时仍保存更新后的人设/工具指纹与空值状态；诊断模式继续完成采集，严格对话模式仍停止。
- 报告记录工具版本和脚本 SHA-256，便于判断是否混用了脚本。
- 空人设/空工具的相等指纹标记为 `match_with_empty_fields`，不能当作保存 Agent 已完整加载。非空指纹相同也不证明 Agent 身份；仍需后台记录、已返回的 Agent ID（如果有）、人设/音色/工具行为核对。

## 报告看哪里

| 字段 | 含义 |
|---|---|
| `tool_version` / `script_sha256` | 版本与实际执行脚本指纹 |
| `test_scope=session_only` | 本次没有执行问答 |
| `results.*.connected` / `token_issued` | 握手成功 / 短期令牌签发成功；不等于完整 Agent 验收 |
| `results.*.updated_audio_evidence.locations` | 已知音频字段的实际类型及白名单值 |
| `results.*.audio_configured` / `audio_validation` / `audio_error` | 音频是否确认、依据、失败原因 |
| `results.*.updated_config_metadata` | 人设、工具等是空、非空、缺失还是 null，不含正文 |
| `results.*.config_error` | 音频设置前后配置改变，或服务端返回了不同 Agent ID |
| `config_comparison` | 优先比较更新后的配置；相等不代表后台 Agent 已正确加载 |

诊断显示字段缺失不等于 xAI 没有音频能力。不要反复换 Key、猜采样率，或把 API Key 填进设备。

## 诊断明确后，才做一轮语音测试

在工具文件夹打开 CMD，严格模式：

```bat
.venv\Scripts\python.exe verify_agent.py --auth key --rounds 1 --play
```

只有诊断明确显示旧式 `pcm16`、没有冲突元数据且接受采样率推定时，才使用：

```bat
.venv\Scripts\python.exe verify_agent.py --auth key --rounds 1 --allow-legacy-pcm16 --play
```

该开关不接受未知/null 格式。旧式 PCM16 未显式返回采样率时，只按文档默认值**推定** 24 kHz，报告标记 `legacy_pcm16_rate_24000_assumed`；这不是采样率实测。声音异常时停止，不继续多轮。严格模式确认明确 PCM24k 后可对照两种鉴权：`--auth compare --rounds 1 --play`。

确认一轮文字和音频正常、人设与音色正确之后，再运行 `--auth compare --rounds 10 --play`。第 7/8 轮尝试触发后台已有 Web/X 工具；模型声称“搜索了”不算工具调用证据。`--cancel-probe` 是额外的取消协议试验，不是自然打断；可能遇到回复已结束的竞态。

## 保护配置和凭证

- 请求路径只有 `wss://api.x.ai/v1/realtime?agent_id=<YOUR_AGENT_ID>`，没有 model 参数或自动退回通用 Grok。
- 只设置输出为 PCM24k、JSON 分片，不覆盖 model / voice / instructions / tools / turn_detection。
- 长期 Key、短期令牌、Agent ID 仅通过隐藏输入或本机 `XAI_API_KEY` / `XAI_AGENT_ID` 环境变量取得，不写文件或命令行参数。不会创建 `.env` 或监听端口。
- 不打印原始错误正文、事件、鉴权头、会话 URL 或 traceback。报告只含结构证据和配置指纹；已知凭证额外脱敏。
- 对话模式会在终端显示回复，WAV 可能包含个人设置内容，分享前自行检查；results 已从 Git 排除。
- TLS 校验保留，HTTP 不跟随跳转。客户端自定义函数没有真实处理器时停止，不伪造工具成功。

尚未验证：麦克风输入、真实播放中的取消、设备独立联网、AEC/VAD、自然打断、恢复固件和云端设备认证/防重放/限流。此次不改设备、不部署服务。电脑仅在验证期间需要运行，最终目标仍是不依赖电脑。

## 开发者验证

```sh
python -m pip install -r requirements.txt
python -m unittest discover -s . -p 'test_*.py' -v
```

离线测试只使用模拟 HTTP/WebSocket，不证明真实 API 已修复。CI 在 Linux 和 Windows 运行，并在 Windows 检查启动器发现 Python 的流程。

协议依据：[xAI 短期令牌](https://docs.x.ai/developers/model-capabilities/audio/ephemeral-tokens)、[Speech to Speech](https://docs.x.ai/developers/model-capabilities/audio/speech-to-speech)。保存 Agent 路径依据用户 Console Deployment 的官方示例。文档规定 PCM 默认 24 kHz；默认值与真实会话明确返回值应分别记录。
