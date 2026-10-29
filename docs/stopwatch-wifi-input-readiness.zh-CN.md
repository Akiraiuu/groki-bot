# StopWatch：纯 Wi-Fi 会话输入就绪状态

## 行为

本地 GET /api/voice-test 增加布尔 input_ready。它只表示当前已授权的付费会话输入链路就绪，不授予任何费用权限，也不启动连接。

必须同时满足以下条件才可首次置 true：
- 当前 paid 会话为 Running，授权 nonce 与会话启动时捕获的 nonce 一致，尚未超时。
- 云端协议已完成 setup 并进入 Listening。
- 两个麦克风队列槽均已成功启动，预热样本已处理完毕。
- 音频帧已被客户端发送队列接受，下一块录音成功入队，单轮输入仍开放。

发送队列接受只证明设备端已接受该帧，不证明云端 ASR 已收悉或识别。物理录音的实机质量仍须独立证据。

结束输入、开始播放、停止、失败、超时、finish、禁用及新会话开始均清除就绪状态。旧 nonce 的迟到更新无法设置或清除新会话的状态。生命周期中的 input_ready 和 approval_id 来自同一互斥快照。

设备屏幕的“收音已就绪”和串口 session ready 日志推迟到同一条件成立后发出；传输 setup 被接受时的早期日志改为 transport setup accepted。

任一初始录音槽或后续补充录音失败时，当前 Grok 会话停止为 Error，避免在麦克风不可用时继续等待输入。其他提供方沿用现有流程。

## 主机使用约定

只有新授权、预期固件、mode=paid、phase=running、input_ready=true、approval_id 等于本轮 START 后的 nonce，且剩余预算足够时，主机才可向用户发出说话选项。phase=running 或旧串口日志都不能单独作为输入就绪依据。

主机仍必须遵守独立单次 start、包含准备的总时限、完成/取消/超时/失败停止及回读验证；不自动重连或重试。

## 验证范围

test_paid_voice_test.cpp 新增就绪状态生命周期、旧 nonce、截止时间、重启、Local 模式隔离和并发停止用例；使用现有 paid_voice_test 主机测试目标。

当前新增源码尚待 C++ 主机回归与 C152 编译。此说明不构成安装、免费收音或付费测试授权。
