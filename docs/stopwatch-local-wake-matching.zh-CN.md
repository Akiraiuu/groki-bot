# 完整短语匹配与无音频诊断

保持目标“你好，精通”（ni hao jing tong）不变，移除与它重叠的短命令“ni hao”“jing tong”。这些短命令原先被作为拒绝项，但模型若先返回它们，应用随后clean会重置识别上下文，可能使完整短语被拆开。保留不重叠的拒绝项“介绍一下自己”“再见”。这是消除匹配配置冲突，不代表已经证明某次漏检的唯一根因。

采用模型原生默认检测策略，不再设置额外阈值或要求结果分数达到0.90。仅在模型返回ESP_MN_STATE_DETECTED时，检查有效结果数1..5、最佳候选command_id=1、分数有限且在(0,1]，才报告本地唤醒；仍拒绝其他指令。模型分数不能直接作为经过校准的正确率，不同检测策略不能简单共用一个应用层百分比阈值。原16kHz/16bit/mono、预热、40秒限时、独立内部栈任务、资源释放顺序以及单独费用确认全部保留。触发本地费用确认不等于授权云端连接。

本地HTTP状态新增数值统计：frames、samples_per_frame、peak、mean_abs_peak、clipped_samples、timeouts、rejected、detect_max_us、last_command、best_target_per_mille、best_other_per_mille。每轮模型启动时清零，结束后保留，便于区分静音/削波、帧处理、模型未给出候选和门槛拒绝。last_command只包含预设命令编号；概率按千分比输出，不是经过校准的准确率。

统计不存储或传输PCM、录音、识别字符串或对话内容。模型自检仍不操作麦克风；所有硬件验证需新就绪确认，任何付费调用需单独确认。主机回归覆盖完整短语存在、禁止前后缀竞争项、自检零音频统计、信号统计、INT16_MIN幅度，以及其他命令、NaN/无穷/零/负/越界分数和无效结果数量拒绝；原生检测的有效中等分数目标必须在资源释放后返回成功。主机替身测试不能替代真实语音识别准确率和误唤醒验收。

API/输入格式参考：
https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html
