# 受控语音事件投递候选：容量预留、首因保留与批次预算

本改动是分阶段候选，不是播放质量、独立关停或硬件验收结论。默认关闭、明确授权、限轮／限时及禁止自动重连规则不变；开机、OTA、Wi-Fi恢复不会授予付费权限。

## 已接入的事件路径

- Grok受控路径使用独立32项FIFO。普通音频／文本增量等payload最多占24份入场许可，给控制事件预留容量。容量值仍需SDK调度验证。
- 许可由队列封装对象以RAII持有；入队失败、分配失败、消费和清空都归还一次，不提前释放仍在队列中的对象。
- Error的有限错误码直接写入会话对应的首因，不依赖可满的事件队列；回调不获取PaidVoiceTest互斥量、不操作麦克风、不重连、不保存错误原文。
- payload或总容量耗尽、对象分配／底层入队失败会产生明确失败状态。不得把丢失数据后的回答当成正常完成。
- 新固定fault_source为event_inbox，fault_detail仍为-1；源ConversationError使用原conversation_event及0..7／未知-1规则。STOP／finish不覆盖首因，新明确授权才清空。
- 回调捕获会话generation；旧回调和旧撤销不能操作新范围。新范围只有在旧生产者静默、已有封装对象全部归还后才能开始。
- 保留单一FIFO顺序，不把AudioDone或ResponseDone提前到尚未消费的音频之前。
- 事件处理采用每批最多4项／两事件之间检查2ms预算，之后返回播放服务。它不能抢占一个正在阻塞的处理函数，参数不是硬件时延保证。
- 资源不变量不满足时禁用新会话、保留Stopping及资源所有权并提示关机，不伪造finish/idle。
- 其他提供方保留原队列及连接事件保留路径。

## 目标平台状态同步修订

事件策略不再要求std::atomic<uint32_t>具有无锁实现。ESP_PLATFORM下通过SDK任务临界区保护少量状态，主机通过std::mutex保护同一组状态；代次检查、容量及首因更新在同一状态锁中完成。ESP spinlock使用显式DRAM静态存储，避免依赖Coordinator的堆分配位置。

无锁支持取决于编译配置。不能只用目标编译器默认参数验证该假设；带`-mdisable-hardware-atomics`的配置需要独立检查。本修订使用显式状态同步，不再把标准库无锁常量当作所有目标配置的前提。

临界区只读写固定大小状态，不分配／释放事件对象、不调用队列、音频、网络、日志、PaidVoiceTest或其他锁，也没有重试循环。许可封装对象在锁外构造／销毁，计数归还在短锁内完成；begin仍须由调用者先确认旧生产者静默，不能以撤销或超时代替。

SDK获取spinlock可能等待另一核；此修订不宣称回调无锁、等待有硬上限或底层停滞时能独立关闭。仅支持现有任务上下文回调，不能将该入口直接用于ISR。主机ESP分支测试使用接口桩，不能证明Xtensa编译、DRAM链接放置或实际调度时延；目标构建和SDK关停适配仍需验证。

接口依据：ESP-IDF v5.5.6 FreeRTOS critical sections：
https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-reference/system/freertos_idf.html#critical-sections

## 关停策略的明确限制（继续保留）

voice_control_policy.hpp还提供CloseOwnershipPolicy原型，分别要求权限撤销、音频停止、发送线程结束、传输关闭、回调静默、关停工作线程结束及事件队列清空的真实确认。超时不授权释放，迟到完成仍标ClosedLate，不能擦除已错过的截止。

该原型尚未接入ESP WebSocket／RTOS关停适配。本改动不改变原生teardown等待，也不能保证底层停滞时独立完成关闭。不得仅因策略测试通过就发布、安装或宣称自动关停问题已解决；仍需完成SDK适配与构建验证。

## 验证

无设备C++主机测试：

- voice_control_policy：容量预留、首因、generation、预算、所有权、截止以及实际多线程申请／释放。
- event_inbox：FIFO、控制事件容量、显式过载、对象分配／入队失败回滚、真实PaidVoiceTest状态的首因映射及STOP／finish保留。
- voice_control_policy_esp_boundary：同一策略在ESP_PLATFORM分支及任务接口桩下执行，检查临界区不嵌套、所有出口平衡、对象复制／分配及队列操作位于锁外。策略测试另覆盖真实多线程撤销竞争和旧代次故障隔离。

测试只使用本地合成数据，不访问设备、请求令牌或调用付费API。主机回归不替代ESP-IDF编译、SDK线程／音频驱动或硬件验收；没有自动语音测试入口。
