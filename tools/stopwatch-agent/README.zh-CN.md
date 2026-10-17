# StopWatch AI Assistant：电脑端 P1 验证

本工具验证你在 xAI Console 发布的 **StopWatch AI Assistant**，先确定「保存的 Agent + 短期令牌」可行，再改 StopWatch 固件。不是新固件，也不是云端令牌服务。

## Windows 操作

1. 安装 [Python 3.11 或更新版本](https://www.python.org/downloads/windows/)，安装时保留 Python Launcher（`py`）。已安装则跳过。
2. 解压工具包到普通文件夹。不要直接在 ZIP 里运行。
3. 双击 `run-windows.cmd`。首次运行会创建本地虚拟环境，并安装 `requirements.txt` 中固定版本的依赖。
4. 在自己的电脑窗口中粘贴 **xAI API Key**，按回车。输入内容不显示，这是正常的。不要发到聊天或写进源码。
5. 打开 xAI Console → StopWatch AI Assistant → Deployment → Python，从示例的连接地址中复制 `agent_id=` 后面的完整值（形如 `agent_...`，不含引号、逗号），粘贴到窗口，按回车。仍为隐藏输入。
6. 工具依次用长期 Key 和刚申请的短期令牌连接同一个 Agent；各发送 **10 轮**中英文测试问题，Windows 自动播放每轮完整回复，会使用 xAI API 额度。首次先验证这一步；需要时可按下文命令另外开启取消协议探测。
7. 完成后打开 `results/日期时间/`，检查 `report.json` 和两组 WAV。先反馈报告中两种鉴权的 `protocol_ok`、`error`（如果有）、`config_comparison.status`，以及试听是否与 Console 一致。

电脑只在这一步用于验证。最终设备方案仍是 **StopWatch → xAI，云端只签发短期令牌**。

## 测什么、怎么判断

| 项目 | 证据与边界 |
|---|---|
| 指定保存的 Agent | 两种鉴权都使用 `wss://api.x.ai/v1/realtime?agent_id=<YOUR_AGENT_ID>`；没有 model 参数或通用 Grok 回退 |
| 保留后台配置 | 不发送 model / voice / instructions / tools / turn_detection；只设置输出为 24 kHz PCM16 单声道、JSON 分片 |
| 短期令牌 | 本机 Python 用长期 Key 请求 `POST /v1/realtime/client_secrets`，请求只含 `expires_after.seconds=300`，随后立即用返回的 `value` 连 Agent |
| 连续问答 | 每种鉴权在同一条连接中完成 10 轮，保留上下文；要求正常结束、收到音频和文字分片 |
| 配置比较 | 只对服务端披露的 model / voice / instructions / tools 求 SHA-256，不保存内容。`match` 仅证明两次披露值相同，不证明 Agent 身份或所有行为；字段不完整时为 `partial` |
| 配置保护 | 如果只修改音频格式就导致可比较的人设、音色等字段改变，立即失败，避免继续覆盖配置 |
| 人设和音色 | **人工对照** Console 的 Try it live。报告始终保留 `pending`，不把连接成功冒充人工验收 |
| Web / X 搜索 | 第 7/8 轮尝试触发后台已启用的工具；只记录事件类型及计数。回答声称“搜索过”不算证据；需结合实际工具事件和 Console 会话记录确认 |
| 取消（可选） | 加 `--cancel-probe` 后，在倒数第二轮后额外发起回复，收到首段音频后发送 `response.cancel`，检查 `cancelled`，再做下一轮。`completed` 可能是服务端已经生成完成的竞态，只能记为未证实。没有实时扬声器队列，不测自然打断延迟；`--rounds 1` 时不做取消后的下一轮 |

**这只是 P1 的一部分**：默认文本输入、音频输出；尚未验证麦克风输入、真实播放中的取消、设备 AEC/VAD、免唤醒插话、设备重连、云端鉴权及限流。全部 `protocol_ok=true` 也不代表这些功能通过。

如果 Agent 需要客户端执行自定义函数，工具会报告 `client_function_handler_required`，不伪造工具结果。Web/X 等服务端内置工具由 xAI 执行。

## 密钥与结果

- 长期 Key 和短期令牌只在本机进程内使用；不创建 `.env`、密钥文件或监听端口。窗口关闭后再次运行需要重新输入（除非你自己设置了环境变量）。
- 不打印原始 HTTP 错误、WebSocket URL、鉴权头、会话正文或 traceback。已知 Key、令牌和 Agent ID 会从文字输出及报告中脱敏。
- 终端会显示测试问答，WAV 是真实回复，可能包含个人设置内容。只保存在本机；分享前检查，**不要上传整个 results 文件夹**。结果目录已从 Git 排除。
- 不跳过 TLS 证书校验；HTTP 不跟随跳转，不将 Key 自动转发到其他主机。
- 这不是设备端密钥方案。未来云端服务必须另做设备认证、防重放、限流、撤销/轮换；当前没有部署任何服务。

## 手动运行 / 快速排错

在此文件夹打开终端：

```powershell
py -3 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe verify_agent.py --auth compare --rounds 1 --play
```

上面只做每种鉴权各一轮的快速连通测试，不能代替十轮验证。只测短期令牌：`--auth ephemeral`。完整测试：`--auth compare --rounds 10 --play --cancel-probe`。开发者可以使用 `XAI_API_KEY` / `XAI_AGENT_ID` 环境变量；不支持把密钥写在命令行参数里。

| 结果 | 下一步 |
|---|---|
| `token / http_401` 或 `http_403` | 在 Console 检查当前团队、Key 权限和有效性；不向他人发送 Key |
| `websocket / http_401` 或 `http_403` | 对照 Key 方式与短期令牌方式；只在后者失败时，不能断言保存的 Agent 支持该令牌 |
| `token / unexpected_response_shape` | 官方返回结构与当前解析不符；停在此阶段，不能退回把长期 Key 写进设备 |
| `session / pcm24k_not_confirmed` | 服务端没有确认预期音频格式；不猜测采样率、不生成假成功音频 |
| `events / server_error_check_console` | 查看自己 Console 对应时间的请求/会话错误；工具不会输出可能含私密信息的错误正文 |
| `config_comparison.status=partial` | 服务端未完整披露配置，需要继续通过后台记录和试听验证，不视为完整配置匹配 |

## 开发者离线测试

```sh
python -m pip install -r requirements.txt
python -m unittest discover -s . -p 'test_*.py' -v
```

测试用模拟 HTTP / WebSocket，不接触真实 xAI、不使用真实密钥。覆盖令牌请求、失败不回退、配置保护、旧音频过滤、取消、10 轮对照、WAV 参数及脱敏。

协议依据：[xAI 短期令牌](https://docs.x.ai/developers/model-capabilities/audio/ephemeral-tokens)、[Speech to Speech](https://docs.x.ai/developers/model-capabilities/audio/speech-to-speech)。保存 Agent 的 `agent_id` 路径来自用户 Console Deployment 的官方 Python 示例；短期令牌与该路径的组合需要本机真实请求验证。
