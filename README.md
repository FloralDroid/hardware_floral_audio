# Floral Audio HAL

[English](#english) | [中文](#中文)

## English

`hardware_floral_audio` provides the primary physical speaker output for
FloralDroid. AudioFlinger mixes application and system playback into fixed
48 kHz, stereo, signed 16-bit PCM. The HAL publishes that final mix to
`floral_device_service` through the `floral.device.audio` stable AIDL and a
synchronized FMQ.

The HAL does not encode audio and does not open a host socket. FloralDevice
owns Opus encoding, dynamic bitrate control, FSA1 framing, and
`/mnt/vendor/floral_stream/audio.sock`.

### Current scope

- Legacy `audio.primary.floral` module wrapped by Android 12
  `android.hardware.audio@7.0-impl`.
- Output only; microphone input is intentionally unsupported.
- 48 kHz, stereo, PCM S16_LE.
- 240-frame blocks, representing 5 milliseconds of audio.
- Four-block bounded local queue followed by an eight-record synchronized FMQ.
- Oldest local-block drop and discontinuity signaling under backpressure.
- Monotonic frame position and first-sample `CLOCK_MONOTONIC` presentation time.

`out_write()` performs only virtual-device pacing and bounded memory copies. A
dedicated publisher thread discovers `floral.device.audio.IAudioPcmSink/default`,
opens the FMQ, and publishes fixed FAP1 PCM records. Binder discovery and FMQ
reconnection never run on AudioFlinger's write path.

When FloralDevice is absent or falls behind, AudioFlinger timing continues and
bounded audio blocks are discarded. The next published record carries a
discontinuity marker so the Opus session resets its predictor state.

### Product integration

Map this repository to `hardware/floral/audio` and inherit `audio.mk`. It
packages `audio.primary.floral` and selects it with:

```text
ro.hardware.audio.primary=floral
```

The redroid product's existing 48 kHz primary policy remains the single owner
of the installed audio policy files. The previous second module installing the
same `primary_audio_policy_configuration.xml` was removed.

The internal FMQ record is documented in [PROTOCOL.md](PROTOCOL.md). The FSA1
host protocol belongs to `packages_services_FloralDevice/PROTOCOL.md`.

### Security boundary

The HAL receives the final AudioFlinger primary mix in the same role as a
physical speaker. It does not use MediaProjection, AudioPlaybackCapture, or a
recording API. DRM and protected-audio behavior are not claimed by the current
contract and require separate validation.

## 中文

`hardware_floral_audio` 为 FloralDroid 提供主物理扬声器输出。AudioFlinger
先把应用与系统播放混合为固定的 48 kHz、双声道、有符号 16 位 PCM，HAL 再通过
稳定 AIDL `floral.device.audio` 和同步 FMQ 将最终混音发布给
`floral_device_service`。

HAL 不编码音频，也不打开宿主 Socket。Opus 编码、动态码率、FSA1 封装和
`/mnt/vendor/floral_stream/audio.sock` 均由 FloralDevice 负责。

### 当前范围

- 提供传统 `audio.primary.floral` 模块，由 Android 12
  `android.hardware.audio@7.0-impl` 包装。
- 仅支持输出；麦克风输入明确不属于当前范围。
- 固定 48 kHz、双声道、PCM S16_LE。
- 每块 240 帧，即 5 毫秒音频。
- HAL 内四块有界队列，后接容量为八条记录的同步 FMQ。
- 出现背压时丢弃最旧的本地 PCM 块，并标记不连续。
- 保留单调采样位置和首采样点 `CLOCK_MONOTONIC` 显示时间戳。

`out_write()` 只执行虚拟设备节拍和有界内存复制。独立发布线程发现
`floral.device.audio.IAudioPcmSink/default`，打开 FMQ 并发布固定 FAP1 PCM
记录。Binder 发现和 FMQ 重连不会进入 AudioFlinger 写入路径。

FloralDevice 不存在或处理落后时，AudioFlinger 时钟继续推进，超出边界的音频块
会被丢弃。下一条成功发布的记录携带不连续标志，使 Opus 会话重置预测状态。

### 产品集成

将本仓库映射到 `hardware/floral/audio` 并继承 `audio.mk`。产品打包
`audio.primary.floral`，并通过以下属性选择模块：

```text
ro.hardware.audio.primary=floral
```

redroid 产品已有的 48 kHz primary policy 继续作为已安装音频策略文件的唯一
所有者。此前第二个同名安装 `primary_audio_policy_configuration.xml` 的模块已移除。

容器内部 FMQ 记录见 [PROTOCOL.md](PROTOCOL.md)；FSA1 宿主协议归
`packages_services_FloralDevice/PROTOCOL.md` 管理。

### 安全边界

HAL 以物理扬声器相同的角色接收 AudioFlinger 最终主混音，不使用
MediaProjection、AudioPlaybackCapture 或录音 API。当前能力契约不承诺 DRM
及受保护音频行为，必须单独验证。
