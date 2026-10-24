# 完整短语匹配与无音频诊断

保持目标“你好，精通”（ni hao jing tong）不变，移除与它重叠的短命令“ni hao”“jing tong”。这些短命令原先被作为拒绝项，但模型若先返回它们，应用随后clean会重置识别上下文，可能使完整短语被拆开。保留不重叠的拒绝项“介绍一下自己”“再见”。这是消除匹配配置冲突，不代表已经证明某次漏检的唯一根因。

仍要求最佳候选command_id=1、概率有限且至少0.90；未降低检测门槛，也不接受其他指令。原16kHz/16bit/mono、预热、40秒限时、独立内部栈任务、资源释放顺序以及单独费用确认全部保留。

本地HTTP状态新增数值统计：frames、samples_per_frame、peak、mean_abs_peak、clipped_samples、timeouts、rejected、detect_max_us、last_command、best_target_per_mille、best_other_per_mille。每轮模型启动时清零，结束后保留，便于区分静音/削波、帧处理、模型未给出候选和门槛拒绝。last_command只包含预设命令编号；概率按千分比输出，不是经过校准的准确率。

统计不存储或传输PCM、录音、识别字符串或对话内容。模型自检仍不操作麦克风；所有硬件验证需新就绪确认，任何付费调用需单独确认。主机回归覆盖完整短语存在、禁止前后缀竞争项、自检零音频统计、信号统计、INT16_MIN幅度、其他命令/NaN/低分拒绝和全词高分释放后返回。主机替身测试不能替代真实语音识别准确率验收。

API/输入格式参考：
https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html
