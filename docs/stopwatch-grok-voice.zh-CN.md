# StopWatch 直连 Grok Voice（二次开发）

## 当前阶段：先验证已发布的 StopWatch AI Assistant

目标已明确为完整沿用 xAI Console 中发布的 **StopWatch AI Assistant**，通过 `agent_id` 接入，由云端签发短期令牌，长期 xAI API Key 不进入设备固件、NVS 或网页设置项。

**现有 `grok-dev-b01942db` 固件是早期通用 Grok 原型，不满足该目标**：它没有 `agent_id` / 云端令牌接入，会发送本地人设、音色和工具配置，而且仍是半双工。请先使用 [Windows 电脑端验证工具](../tools/stopwatch-agent/README.zh-CN.md) 检查保存 Agent 与短期令牌的兼容性；不需要重新刷机，也不需要再向设备填长期 Key。

目前没有真实账号的程序化验证结果，不声称 P1 或独立语音通过。设备设置页可打开，但白点显示现象、音频、恢复到原厂固件仍待实机验证。继续刷写前应备妥与 C152 对应的恢复固件及恢复步骤。以下保留为**早期原型的历史说明**，不作为最终配置流程。

## 早期原型

这条开发分支基于 `JosssphZhou/groki-bot`，目标板为 **M5Stack StopWatch C152**。沿用上游的表情、口型、设置、OTA、重连及触摸屏交互，新增 xAI Grok Voice 服务商。首次刷机、配置完成后，设备通过 Wi-Fi 直接连接 xAI，无须常开电脑、Python 网关或小智服务器。

## 配置

1. 编译并刷入本分支的 **stopwatch** 固件。上游已发布的固件没有本次新增功能，其他板型固件也不能混刷。
2. 配置可访问 `api.x.ai` 的 2.4 GHz Wi-Fi。
3. 打开设备的 Wi-Fi 设置页 `http://stackchan-XXXXXX.local/`（也可使用屏幕显示的 IP；AP 模式可以从手机打开设置）。在“会話 / 对话”页选择 **xAI Grok Voice**，填写 **xAI API 密钥**。密钥来自 [xAI Console](https://console.x.ai/)，OpenAI / Gemini 密钥及 Grok App 订阅不能替代它。
4. 将运行模式设为 **Conversation / AI 对话**，开启 **触摸打断 / barge-in**。该开关沿用上游的默认值，未开启时轻触不会打断。
5. 可在系统提示词中填写“请用中文简短回答。”，保存并重启。设备会连接 xAI，开始采集语音。
6. 播放回复时轻触头像屏幕，停止播放并恢复聆听，再说下一句话。

BLE 配置也可使用本分支 `tools/settings.html`。页面须在 Chrome / Edge 支持 Web Bluetooth 的安全上下文中打开。新增密钥通过已有加密会话写入设备；旧版 BLE 设置页不包含 Grok 选项。BLE 页面主要负责连接和密钥，系统提示词可在设备 Wi-Fi 页面配置。

密钥按服务商独立保存在 NVS，不编译进固件；设置页留空表示保留已有密钥。HTTP 状态仅返回 `has_xai_key`，不会返回密钥内容。开发接口新增 `POST /api/xai-api-key`，`provider` 的新增值为 `3`；现有 `0`、`1`、`2` 不变。批量设置接口也通过注册表支持新字段。BLE 新增写入特征 UUID：`e3f0a02f-7b1c-4d2a-9e6f-2c5a8d4b1f00`。

## 编译

按照根目录 README 安装 ESP-IDF 5.5，并准备 Node.js 18+：

```sh
git submodule update --init --recursive
tools/apply-m5-patches.sh
make build BOARD=stopwatch
make flash BOARD=stopwatch PORT=/dev/ttyACM0
```

也可使用项目现有 Docker 构建：`make build-docker BOARD=stopwatch`。输出目录为 `build-stopwatch/`。仅刷自己编译的产物时，可参考该目录的 `flash_args` / `flasher_args.json`，不要使用上游在线刷机页代替本分支固件。

GitHub PR 会触发新增的 **StopWatch development firmware** 构建检查；通过后可从对应 Actions 运行的 Artifacts 下载 `stopwatch-grok-voice-<sha>`。构建只产出开发固件，不会自动发布或烧录。原有 **Build and Release Firmware** 工作流也可手动运行。

默认模型为 `grok-voice-latest`，音色为 `eve`。可通过 `CONFIG_STACKCHAN_XAI_REALTIME_MODEL` / `CONFIG_STACKCHAN_XAI_VOICE` 编译选项修改。两者与 OpenAI 的模型、音色配置独立。

## 行为与验证边界

- 按 [xAI 官方语音协议](https://docs.x.ai/developers/model-capabilities/audio/speech-to-speech)连接 `wss://api.x.ai/v1/realtime`，使用服务端 VAD 检测句子结束。会话配置确认后才开始上传音频。
- 第一版复用上游的 **8 kHz G.711 μ-law** 音频管线，以降低 ESP32 的网络和内存负载；音质属于电话带宽。
- 第一版支持**触摸打断**。现有半双工逻辑在播放时暂停麦克风采集，因此不声称支持“直接开口覆盖 AI 声音”的全双工语音打断。
- 打断时停止本地播放、请求取消仍在生成的回复，并丢弃已排队和随后抵达的旧回复内容。Wi-Fi 断线沿用上游的重连流程；此版本重新连接会开始新会话，不恢复云端历史。
- 尚需真实 StopWatch 和有效 xAI API 密钥验证：中文对话、播放中的连续打断、下一轮语音、断网重连、长时间运行。协议单元测试或编译成功不能代替这些实机测试。

主机测试（使用 CMake、C++20 编译器、mbedTLS 开发包，例如 Debian/Ubuntu 的 `libmbedtls-dev`，已初始化 `tl_expected` 子模块）：

```sh
cmake -S components/conversation/test/host -B build-conversation-host
cmake --build build-conversation-host
ctest --test-dir build-conversation-host --output-on-failure
```

本次云环境已通过 3 项主机测试（ASan + UBSan）：原有小智控制分发、Grok/OpenAI 会话和独立密钥配置、使用模拟 WebSocket/RTOS 的真实语音客户端。客户端测试覆盖会话确认、μ-law 音频解码、取消后的旧音频/文本/工具过滤、取消后继续对话、已完成回复不重复取消、错误事件及重新连接。模拟测试不验证真实网络、板载音频或触摸硬件。两个设置页也通过 JavaScript 语法检查。

代码版本 `9954f5a` 已通过 [StopWatch C152 完整构建](https://github.com/Akiraiuu/groki-bot/actions/runs/39054131633)，可[下载该版本开发固件](https://github.com/Akiraiuu/groki-bot/actions/runs/39054131633/artifacts/12108389841)（需登录 GitHub）。后续构建结果请查看 [PR #1](https://github.com/Akiraiuu/groki-bot/pull/1) 的 **StopWatch development firmware** 检查。测试通过或源码上传本身不代表固件可用；只有该构建成功并上传 Artifacts 后才有对应开发固件，仍需完成实机验证。

构建产物下载后先解压：

| 文件 | 用途 |
|---|---|
| `firmware-stopwatch-grok.bin` | 含引导程序、分区表和应用的合并镜像，USB 烧录起始地址为 `0x0`，仅适用于 C152 |
| `stackchan_idf.bin` | 应用镜像；已运行兼容 StopWatch 固件时，用于本地 OTA 上传 |
| `flash_args` / `flasher_args.json` | 分区烧录地址及参数；分别烧录各分区时使用，不能把应用镜像当合并镜像使用 |

这些文件属于开发固件。完整编译证明构建和分区大小检查通过，不证明设备、麦克风、播放、联网和 xAI 账号已联调成功。
