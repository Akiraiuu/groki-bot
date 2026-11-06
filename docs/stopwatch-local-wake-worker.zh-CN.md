# 本地识别任务、无收音自检与停止确认

模型 mmap/create/commands/destroy/unmap 统一运行在 core0 的低优先级独立
任务，栈为16KiB INTERNAL|8BIT。TLS协调任务保留原8KiB PSRAM栈。模型任务
的句柄、静态完成信号量和输入帧也使用内部RAM；不能分配时安全返回不可用。
开始前检查内部/PSRAM预算。固定ESP-SR 2.6.0，即上一构建实际解析的版本。

这消除了模型初始化与原PSRAM栈直接共用的资源风险，但未获取旧故障回溯，
不能声称原重启根因已证实。RTC保留最后初始化阶段，日志记录阶段、可用
内存和栈余量。单独发布ELF/map调试产物用于必要时解码回溯，不进入安装镜像。

模型任务必须先结束麦克风、擦除PCM、释放命令表/解码器并取消映射，再发送
完成信号。协调任务见到完成信号后删除同核任务并释放内部栈，之后才允许
把I2S交给本地回放或已明确授权的付费会话。close为非阻塞；未完成即继续
等待，不得启动云端。低优先级任务让HTTP和停止控制保持可调度。

开启唤醒从用户点击起最多40秒，包含模型初始化和预热；设备本身计时，
不依赖电脑/网络来实现截止。取消同样由模型任务自行清理。40秒到期后
关闭本地识别，不自动重启。已经出现的费用确认仍单独受20秒期限约束。

设备“对话”页新增“模型自检（不收音）”：最多20秒，仅加载模型、建立命令
表、校验帧规格并释放，代码路径不调用麦克风/扬声器。首次排查优先执行
此自检，不能为了复现而直接再次开启收音。硬件自检仍须先确认就绪。

HTTP语音状态增加local_wake对象：enabled、check_only、state、stage、error、
microphone_active、worker_active、remaining_ms、previous_stage、reset_reason、
internal_free、psram_free和stack_free。没有音频、文本内容或凭证。
本机停止请求立即撤销enabled、取消费用弹窗并使原nonce失效。UI仅在
模型任务和麦克风释放、协调任务确认且付费状态idle后显示“已停止”。
重启后的提示改为“精通AI助手：语音未连接”，避免将开机提示误解为付费要求。

主机测试直接编译生产local_wake.cpp，只替换硬件/RTOS边界。覆盖内部栈
创建参数、自检零麦克风操作、完整预热、40秒边界/时钟回绕、内存不足、模型
缺失、任务创建失败、初始化期间停止、完成信号前不释放所有权、无效命令/
NaN置信度拒绝，以及识别返回前释放I2S和模型。不能替代真实模型准确率测试。

官方参考：
https://github.com/espressif/esp-idf/blob/v5.5.6/components/esp_mm/esp_mmu_map.c
https://github.com/espressif/esp-idf/blob/v5.5.6/components/spi_flash/cache_utils.c
https://github.com/espressif/esp-sr/blob/master/src/model_path.c
https://components.espressif.com/components/espressif/esp-sr/versions/2.6.0/readme
