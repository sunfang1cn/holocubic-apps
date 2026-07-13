# airplay_service

`airplay_service` 是 HoloCubic ESP32-S3 的后台 AirPlay 1（RAOP）音频接收服务。设备联网后自动发布 AirPlay 音箱；没有 AirPlay 会话时只监听网络，不启动 I2S，因此原来的本地音乐 App 和启动页面仍可正常使用。

## 已实现

- `_raop._tcp.local` DNS-SD/mDNS 发布和查询响应。
- RTSP：`OPTIONS`、`ANNOUNCE`、`SETUP`、`RECORD`、`SET_PARAMETER`、`GET_PARAMETER`、`FLUSH`、`PAUSE`、`TEARDOWN`。
- audio、control/retransmit、timing/NTP response 三路 UDP。
- Apple-Challenge 的 RSA PKCS#1 v1.5 响应。
- `rsaaeskey` 的 RSA-OAEP/SHA-1 解封装和 AirPlay AES-128-CBC 解密。
- Apple Lossless（ALAC）和未加密 L16/44.1 kHz/16-bit 播放。
- 128 包 PSRAM 抖动缓冲、乱序处理、预读缺口重传、自适应补帧和高水位追帧恢复。
- ALAC/AES 生产者与 I2S 消费者双任务流水线；Lua 网络回调只解析 RTP 头并复制数据。
- 32 包 PCM 环形队列、双核任务分配和 AES 逆变换只读查表优化。
- DMAP 标题、歌手、专辑，播放进度、音量和封面类型/大小解析。
- 左右声道峰值、播放状态和统计数据接口，供音乐 App 后续展示元数据、按曲目匹配歌词或绘制可视化。
- 热重载以及 socket、timer、I2S、任务和解码器释放。

当前仓库已经包含编译好的 `package/modules/airplay_core.so`。该文件使用 ESP-IDF 6.0.2 为 ESP32-S3 构建，动态符号表没有依赖固件内部的 IDF/libc 符号，只通过仓库现有的 `module_abi.h` 使用任务、内存、Lua 和 I2S。

> 状态说明：协议、密码学、交叉编译以及 iPhone 到 HoloCubic 的 ALAC/RSA/AES 真机链路均已验证。当前实现是 AirPlay 1 音频接收器，不支持 AirPlay 2、视频、屏幕镜像、FairPlay 视频或密码配对。

## 部署

把 `package` 的内容复制到 SD 卡：

```text
/sd/apps/airplay_service/
  app.info
  main.lua
  config.lua                  # 可选，由 config.example.lua 复制
  modules/
    airplay_core.so
```

`app.info` 已配置为：

```ini
kind = service
autostart_service = true
```

服务会在 Wi-Fi 获得 IPv4 地址后开始广播。后台 service VM 没有 `wifi` 全局时，会自动读取固件本机 `/api/system/state` 获取 STA 地址。默认名称为 `HoloCubic`，RTSP 端口为 5000，I2S 数据引脚为 48；可以用 `config.lua` 覆盖。也可以在特殊网络环境中用 `ip = "192.168.1.123"` 显式指定地址。

默认实时参数为 48 个 ALAC 包预缓冲（约 384 ms）、32 包 PCM 队列和 12×512 I2S DMA。ALAC/AES 生产者固定在 core 1，I2S 消费者固定在 core 0，优先级分别为 8/9。零星缺包时最多等待 12 个重试周期；PCM 队列降到 24 包后立即停止等待并补一个 8 ms 静音帧，避免长时间 Wi-Fi 阻塞拖空 I2S。RTP 缓冲异常逼近容量时会跳到最新安全窗口，避免进入持续溢出状态。

原生核心需要 PSRAM。固定 RTP 缓冲约占 264 KB，PCM 队列约占 66 KB，此外还有 ALAC 解码器和临时缓冲。服务空闲时不会申请这些流缓冲，也不会占用 I2S。

## UI/状态接口

后台 service 与普通 App 使用隔离 Lua VM。其他 App 应通过本机只读状态接口获取数据：

```text
GET http://127.0.0.1/airplay_service/status
```

返回 JSON 包含 `phase`、标题/歌手/专辑、进度、声道电平、网络状态，以及 `audio` 下的实时指标：

- `buffered_packets`、`received`、`dropped`、`late`、`lost`、`decoded`。
- `written_bytes`、`decode_errors`、`buffer_resyncs`、`pcm_buffered_packets`、`pcm_underruns`。
- `resend_serial`、`missing_wait_ticks`、`codec`、`playing`、`buffering`。

如果调用方与服务处于同一 Lua VM（例如调试时直接加载），也可以使用全局接口：

```lua
local airplay = rawget(_G, "AIRPLAY_SERVICE")
if airplay then
  local state = airplay.get_state()
  print(state.phase, state.title, state.artist, state.position_ms)

  airplay.subscribe(function(next_state)
    if next_state.playing then
      -- 展示 AirPlay 标题、歌手、进度或可视化页面
    else
      -- 保持现有本地文件播放器页面
    end
  end)

  local level = airplay.visualizer()
  print(level.left, level.right, level.buffered_packets)
end
```

状态阶段为 `waiting_for_wifi`、`advertising`、`connecting`、`buffering`、`playing`、`paused`、`error`、`stopped`。

本地播放器和 AirPlay 不能同时占用 I2S。服务提供 `set_audio_focus_handler(fn)`；后续接入 `mp3_player` 时，可由播放器注册暂停/移交音频焦点的函数。如果没有注册处理器且本地歌曲正在播放，服务会拒绝 AirPlay 的 `RECORD` 并设置 `focus_conflict`，避免破坏当前播放。

`visualizer()` 在原生核心启用后还会返回：

- `left`、`right`：0～1 的声道峰值。
- `buffered_packets`、`received`、`dropped`、`late`、`lost`、`decoded`。
- `written_bytes`、`decode_errors`、`codec`、`playing`、`buffering`。

## 构建原生核心

已按本机工具链 `D:\esp\v6.0.2\esp-idf` 配置：

```powershell
cd D:\esp_projects\holocubic-apps\airplay_service\src
cmd /d /c "call D:\esp\v6.0.2\esp-idf\export.bat && idf.py -B build-v6.0.2 reconfigure && ninja -C build-v6.0.2 airplay_core_so"
```

目标会生成 `src/build-v6.0.2/airplay_core.so`，并自动复制到 `package/modules/airplay_core.so`。

核心没有链接 mbedTLS 运行时：AirPlay 1 所需的固定 RSA 运算、PKCS#1、OAEP/SHA-1 和 AES-CBC 是自包含实现；AES 逆 MixColumns 使用 4×256 字节只读查找表，避免逐位有限域乘法成为实时瓶颈。ALAC 使用 `espressif/esp_audio_codec` 2.5.0。这样可以避免动态模块引用与宿主固件版本相关的 IDF 符号。

## 测试

主机密码学测试会验证 SHA-1、NIST AES-CBC 向量、RSA 签名的公钥反算和确定性 OAEP 解封装：

```powershell
python airplay_service/tests/test_crypto_host.py
```

服务加载后可以运行 `probe.lua`，或在 Lua 控制台执行：

```lua
local result = AIRPLAY_SERVICE.self_test()
for _, check in ipairs(result.checks) do
  print(check.ok and "PASS" or "FAIL", check.name)
end
```

电脑端 L16 协议探针会走完 RTSP、DMAP 和 RTP，并播放约 3 秒 440 Hz 测试音：

```powershell
python airplay_service/tools/raop_pcm_probe.py 192.168.1.123
```

## 已知限制

- mDNS 使用 Lua UDP 实现；如果固件自带 mDNS 已占用 5353，服务仍会主动广播，但无法直接接收查询并即时响应。
- 音频时钟目前由接收端 I2S 和 11025-frame latency 缓冲驱动，没有做长期的 NTP/采样率漂移校正。长时间播放需要真机观察是否要加微量重采样。
- 固定抖动缓冲最大接受 2048 字节 RTP payload；标准 AirPlay 1 的 352-frame ALAC 包在此范围内。
- 当前只允许一个发送端会话。

实现过程、真机指标和已验证的失败方案见 [DEVELOPMENT_NOTES.md](DEVELOPMENT_NOTES.md)。
