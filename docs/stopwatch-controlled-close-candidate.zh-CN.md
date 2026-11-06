# StopWatch 受控关闭整体构建候选

本候选将应用会话监督、WebSocket、HTTP、TLS 和 IPv4 解析的退出路径接入同一构建。目前完成主机构建及边界回归，完整 ESP-IDF 固件构建、真实调度、加密握手及硬件验收仍待完成。关闭请求、SDK FINISH 事件或超时不能直接表示资源已经关闭。

## 许可、截止与所有权

付费入口仅在可立即接手的空闲检查点接纳本场请求。原始授权身份、整场截止和操作租约贯穿准备、取票、客户端及音频操作；撤销后拒绝新工作。独立监督在准备操作前建立，沿用原截止并预留关闭预算。任务创建失败、关闭失败或未及时退出时保留所有者、租约和失败状态，禁止下一场；监督不会强删仍在访问资源的任务。

取票仅允许一次同步 HTTPS POST，必须提供当前本场许可和准备租约。预算最多15秒，并受原整场剩余时间限制；禁重定向和认证追加请求。迟到结果作废，不建立语音连接。关闭返回 BUSY 或未知失败时保留 HTTP 上下文、正文及准备租约，不能清除忙碌状态后再次取票。

受控 DNS 使用可取消的 lwIP 消费者回调。调用者、开始投递、DNS 回调和取消投递分别持有引用；实际全部退出才释放 scope 租约。只取消匹配回调及上下文，不影响其他消费者；最后一个消费者退出时沿用原共享 UDP 清理逻辑。较短网络调用截止也传入 TCP/IP 任务，排队回调不能迟到查询。最多两个解析作业，投递失败不重试；未确认取消时保留真实引用。

## SDK、TLS 与音频关闭

WebSocket 使用粘性的 request_stop 和原截止 stop_until。SDK 任务停止、传输关闭和清理完成分别记录；仅同一个 finish_cleanup 可以继续处理 BUSY，不创建第二次关闭尝试。SDK FINISH 在最终传输收尾之前发生，不能代替关闭证据。应用先撤销许可及网络工作，再等待发送者最后访问，最后按照真实任务和传输事实收尾。

TLS 外部 scope 跟踪操作、取消、描述符及所有者寿命。受控 socket 使用非阻塞 I/O，等待按不超过50毫秒的片段检查撤销及原截止，不依靠另一线程 shutdown 解除阻塞。首个关闭失败保持粘性，未退出操作仍持有资源。受控路径仅支持同步 HTTPS、内部 mbedTLS 和 IPv4；受控异步 TLS、纯 TCP、IPv6 及不受支持配置显式拒绝，既有非受控路径保留。

麦克风与扬声器由一个音频所有者串行收尾。必须等准备、应用、客户端和音频租约以及既有唤醒所有者退出，才能触碰对应资源。事件生产者静默后才能排空队列。下列九项齐备、无关闭失败且未错过原截止，才允许发布关闭验证完成：许可撤销、音频停止、发送者退出、SDK传输停止及销毁、回调静默、关闭工作者退出、事件队列排空、全部操作及协调器退出、监督者最后访问结束；全部事实位图为511。

失败或迟到保持 Stopping，禁止新范围。真实迟到确认仍可观察，但不能抹除截止失败或恢复 Idle。原语音首因不被关闭失败覆盖，只有新明确授权才清空历史故障字段。不增加错误原文、转写、回复、音频或凭证存储。

## 固定构建输入

| 输入 | 固定值 |
| --- | --- |
| ESP-IDF | `3ea97e7591b6a83f5d6f8e0cc7f1b16f27b2b855` |
| lwIP 子模块 | `c6f2f878e7b0f86033214b85547d579be43351e3` |
| WebSocket | 1.8.0，六个原版组件文件各有 SHA256 |
| 官方 amd64 构建镜像 | `espressif/idf@sha256:851f136a2c09cf17c13e952b9975606d7d1050be1480b1fd82c7ea4bce78cfb3` |

镜像基于 IDF v5.5.6。固定提交比该标签多一个空提交，根树及工具版本清单一致；工作流仍取得精确提交并核对原件摘要，不把标签相同当作覆盖许可。

首次 IDF 配置前，`tools/controlled-close/prepare_sdk.py` 一次准备六个固定 SDK 原件到被忽略的 `build-inputs/esp_websocket_client`。全部核对成功后才写入；目标已存在即拒绝，不覆盖、不重试。组件 manifest 用相对 override_path 指向此本地组件。随后门禁核对 IDF／lwIP 精确提交和19个文件的原件／候选摘要，再覆盖15个 IDF、2个 lwIP、2个 SDK 文件。原件保留版本后缀备份。重复应用、输入变化、路径越界或摘要不符立即停止。

本地组件避免修改托管组件后又被校验或重新下载，参见[组件 manifest 官方说明](https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/manifest_file.html)。生成的 SDK 输入、备份、工具链、构建产物和本地验证记录不属于待提交源码。

现有 StopWatch PR 工作流接入该准备链，只构建并保存 CI 产物。现有主机工作流增加准备脚本测试，通过原 conversation CMake 运行应用及新增 DNS、HTTP、SDK、解析器边界测试。既有 conversation 的 ASan／UBSan 参数同时覆盖 C 与 C++。本候选未修改发布或 Pages 工作流，没有自动设备或模型调用入口。

## 验证范围

本机实际 conversation CMake 配置、编译、CTest 共22／22通过，含原14项及新增8项。SDK测试执行生产源码提取的21个实际方法；DNS测试执行两个实际方法的四种宏配置；HTTP测试执行四个实际方法；解析器测试编译完整解析器和scope C。SDK／RTOS／HTTP状态边界及DNS执行器仍为替身，不运行完整WebSocket或HTTP perform状态机。主机唤醒夹具仅修复Windows对齐分配／释放的成对兼容性，设备唤醒逻辑未改。

本机使用实际上游mbedTLS 2.28.9 base64及constant_time模块，替代旧的简化base64替身；这不是完整TLS密码学验收。本机未运行ASan／UBSan；CI消毒器结果尚未取得。

SDK准备脚本5项离线文件测试通过。真实保存的六个SDK原件在合成IDF／lwIP目录中完成19文件检查、应用、备份回读及拒绝重复；该检查向门禁传入固定提交作为夹具参数，没有在真实IDF Git工作树或容器中执行CheckOnly。

完整组件编译和链接仍需C152 CI验证。系统不再调度、密码学或驱动停滞等情况，仍不能保证物理连接在整场上限前关闭；此时保留真实未退引用并失败，不伪造关闭。构建成功也不能代替安装、声音、状态接口和多轮行为验收。

## 第三方源码

覆盖文件保留上游版权及许可证标记。Espressif SDK／IDF源码按Apache-2.0，完整文本见 `patches/stopwatch-controlled-close/LICENSE-Apache-2.0.txt`；lwIP文件保留其BSD条款。candidate文件为StopWatch修改版本，修改范围是受控退出、scope租约、可取消IPv4解析、HTTP关闭传播及构建依赖，不能作为未修改上游原件使用。

上游依据：[WebSocket 1.8.0](https://github.com/espressif/esp-protocols/tree/websocket-v1.8.0/components/esp_websocket_client)、[固定IDF](https://github.com/espressif/esp-idf/tree/3ea97e7591b6a83f5d6f8e0cc7f1b16f27b2b855)。
