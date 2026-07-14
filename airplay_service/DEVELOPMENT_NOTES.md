# AirPlay Service 开发记录（2026-07-14～2026-07-15）

本文记录 `airplay_service` 第一阶段的实现、真机调优结论和后续图形 App 接入注意事项。当前版本为 `0.3.15`，目标为 HoloCubic ESP32-S3、ESP-IDF 6.0.2 和 AirPlay 1/RAOP 音频。

## 今日完成

- 新增后台 service 包：空闲时只做网络广播，不占用 I2S，不影响原有本地音乐 App。
- 实现 `_raop._tcp.local` 广播、RTSP 会话、audio/control/timing UDP、重传请求和时间响应。
- 实现 Apple-Challenge、固定 AirPlay 1 RSA 私钥运算、OAEP/SHA-1、AES-128-CBC、ALAC 和 L16。
- 增加 DMAP 标题/歌手/专辑、进度、音量、封面大小/类型和左右声道峰值解析。
- 增加只读状态接口 `GET /airplay_service/status`，供隔离 Lua VM 中的图形 App 获取播放状态和可视化数据。
- 完成 iPhone 真机发现、连接、加密 ALAC 播放以及长时间指标采样。

## 最终实时架构

1. Lua service 负责 mDNS、RTSP、UDP 回调和元数据，不在 Lua 中解密或解码音频。
2. 原生模块把 RTP 放入 512 包 PSRAM 抖动缓冲，预缓冲 160 包。
3. 优先级 10 的生产者完成 AES-CBC、ALAC 和电平计算，把 PCM 放入 256 包队列。
4. core 0 的消费者只负责阻塞式 I2S 写入，避免“解码耗时 + 8 ms 播放耗时”串行相加。
5. 零星缺包最多等待 192 个 2 ms 周期；PCM 队列低于 24 包时取消等待并单帧掩蔽，防止长阻塞形成级联欠载。
6. RTP 序列跨度超过 192 包时跳到最新安全窗口，作为异常网络情况下的最后保护。

关键默认值：

| 参数 | 值 | 目的 |
| --- | ---: | --- |
| RTP slots | 512 | 乱序、重传和网络抖动 |
| RTP prebuffer | 160 包（约 1.28 s） | 启播和 Wi-Fi 抖动余量 |
| PCM slots | 256 包（约 2.05 s） | 解码/I2S 解耦 |
| retransmit floor | 24 包 | 保留约 192 ms 播放安全余量 |
| I2S DMA | 12×512 | 减少短时调度抖动 |
| producer / output core | 任意 / 0 | 双核流水线 |
| producer / output priority | 10 / 11 | I2S 输出优先 |

## 核心坑与结论

### 服务已安装不等于服务已运行

Launcher 能展示 service 只能证明包被扫描到。最初 RTSP 5000 没有监听，是 Lua 5.4 编译错误和后台 VM 环境差异导致服务未启动。诊断时必须同时检查 TCP 端口、状态接口和服务脚本编译结果。

### Lua 5.4 泛型 for 变量是 const

`for _, label in ipairs(...) do label = ... end` 会报 `attempt to assign to const variable`。需要复制到新的局部变量后再修改。

### 后台 service VM 没有 `wifi` 全局

后台 VM 与普通 App 不同。服务通过固件本机 `http://127.0.0.1/api/system/state` 异步读取 STA IPv4，并持续重试；不能假设 `wifi.sta.getip()` 可用。

### 固件 mDNS 已占用 UDP 5353

Lua 服务无法绑定 5353，但仍能从临时 UDP 端口主动发送合法 RAOP 公告，iPhone 可以发现。当前保留警告并继续广播，而不是把地址占用视为致命错误。

### 串行解码和 I2S 无法达到实时速率

旧路径每包先 AES/ALAC、再阻塞写 8 ms I2S，实测只能处理约 77 包/秒，而 44.1 kHz、352 frame 的 AirPlay 流需要约 125 包/秒。结果是 RTP 缓冲持续上涨、周期性追帧和明显破音。双任务 PCM 队列消除了这个结构性瓶颈。

### AES 是复杂音乐/人声段的主要 CPU 热点

原实现每个 AES 字节都用循环计算 GF(2^8) 乘法，ALAC 包越大越容易卡。最终使用 4×256 字节只读乘法表；真机 45 秒采样中 PCM 队列保持 30–32 包，`pcm_underruns=0`、`buffer_resyncs=0`、`decode_errors=0`。

不要恢复曾测试过的“辅助函数版快速逆 MixColumns”。它在 Windows 密码学向量测试中正确，但 ESP32-S3 收到首批加密音频后会使 service VM 重启，iPhone 随即退回本机扬声器。只读查表版经过同一 iPhone 链路验证，不会触发该问题。

### 固定重传等待会造成反向级联

`missing_wait_ticks=1` 会把很快到达的重传包过早判为迟到，产生少量爆音；无条件改成 12 又会在长 Wi-Fi 阻塞中逐包等待，拖空 PCM 队列并造成大量 RTP 覆盖。最终策略必须看 PCM 安全水位：队列充足时等，低于 24 包立即保播放连续性。

### RTSP 响应必须串行发送

连续调用异步 TCP `send()` 会让 iPhone 在约一分钟后主动结束会话并退回本机扬声器。`0.3.10` 起通过 `sent` 回调串行泵送 RTSP 响应，消除了这一类固定时间断开。

### 消费序号必须在 AES/ALAC 之前推进

旧实现清空 RTP slot 后要等约 2.5 ms 的 AES/ALAC 工作结束才推进 `expected_sequence`；这段窗口内到达的重传包会重新填回刚清空的 slot，并在一整圈后形成碰撞。`0.3.15` 在复制编码数据后立即推进序号，再进行解密和解码，解决了该竞争。

### 回调之前的丢包无法由原生音频核心补救

固件不开源，动态模块 ABI 也没有网络/socket 接口，因此 UDP 接收和重传发送都必须经过 Lua 事件循环。前台图形 App 阻塞 Lua 时，数据可能在进入 `airplay_core` 前丢失；AES 加速、ALAC 优化和扩大原生缓冲都无法恢复从未交付给模块的数据。后续音乐图形 App 必须限制帧率、使用局部刷新、限制单帧工作量，并避免阻塞 I/O。

弱 Wi-Fi 会产生相同症状。一次前台无 App 的故障会话中，RSSI 为 -78～-85 dBm，记录到 `lost=5660`、`late=2951`、`pcm_underruns=117`、RTP 最大空洞 504 ms 和输出最大空洞 1.13 s；同期 AES+ALAC 平均约 2.56 ms/包，证明瓶颈在无线到包而非解码算力。诊断时应先把 RSSI 恢复到约 -70 dBm 或更好，再比较服务版本。

## 真机诊断指标

状态接口的 `audio` 对象至少关注：

- `buffered_packets`：RTP 抖动缓冲；持续上涨表示生产者跟不上。
- `pcm_buffered_packets`：PCM 队列；长期接近 0 表示解码或调度余量不足。
- `pcm_underruns`：软件 PCM 欠载；稳定播放期间应保持不变。
- `lost` / `late` / `resend_serial`：无线丢包与重传效果。
- `decode_errors`：ALAC/AES 数据正确性；正常应为 0。
- `buffer_resyncs`：高水位追帧；正常网络下应为 0。

诊断时应看一段时间内的增量，不要只看累计总数。一次启动早期的计数不等于持续问题。

## 构建和发布注意事项

- 使用 `D:\esp\v6.0.2\esp-idf` 和 ESP32-S3 Xtensa 工具链。
- 模块必须是 PIC 共享对象，且 `nm -D -u` 结果为空，不能引用宿主固件未导出的 IDF/libc 符号。
- Espressif ALAC 静态库包含不适合动态加载的 section，CMake 会复制并用 `objcopy` 修补后再链接。
- Windows 环境中 Ninja 偶尔无输出挂起；可用 `ninja -t commands airplay_core_so` 取得 CMake 生成的精确命令并直接执行。
- 发布前必须运行 `python airplay_service/tests/test_crypto_host.py`，再做 iPhone 加密 ALAC 真机测试。

## 明日图形 App 计划

- `mp3_player` 空闲时保持现有本地文件 UI；状态接口 `playing=true` 时切换到 AirPlay Now Playing 页面。
- 展示标题、歌手、专辑、进度和封面；标准 AirPlay 元数据通常不带歌词，可按标题/歌手匹配本地歌词或后续歌词源。
- 使用 `left/right` 峰值做基础频谱/波形动画；若需要真正 FFT，需要在原生核心增加低成本频带统计，避免把 PCM 跨 VM 复制给 Lua。
- 接入音频焦点：本地播放器与 AirPlay 不可同时占用 I2S，UI 切换必须和 `set_audio_focus_handler` 状态一致。
- 增加长时间播放、网络切换、来电/暂停、断线重连和本地播放恢复测试。
