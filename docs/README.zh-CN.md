# Floral Audio HAL

[English](README.md)

`hardware_floral_audio` 为 FloralDroid 提供主物理扬声器输出。AudioFlinger
先把应用与系统播放混合为固定的 48 kHz、双声道、有符号 16 位 PCM，HAL 再通过
稳定 AIDL `floral.device.audio` 和同步 FMQ 将最终混音发布给
`floral_device_service`。

HAL 不编码音频，也不打开宿主 Socket。Opus 编码、动态码率、FSA1 封装和宿主
音频 Socket 均由 FloralDevice 负责。

## 当前范围

- 提供传统 `audio.primary.floral` 模块，由 Android 12
  `android.hardware.audio@7.0-impl` 包装。
- 扬声器输出与虚拟内置麦克风输入可同时工作。
- 固定 48 kHz、PCM S16_LE，输出双声道、输入单声道。
- 每块 240 帧，即 5 毫秒音频。
- 输出路径为 HAL 内四块有界队列，后接容量为八条记录的同步 FMQ。
- 出现背压时丢弃最旧的本地 PCM 块，并标记不连续。
- 保留单调采样位置和首采样点 `CLOCK_MONOTONIC` 显示时间戳。

`out_write()` 只执行虚拟设备节拍和有界内存复制。独立发布线程发现
`floral.device.audio.IAudioPcmSink/default`，打开 FMQ 并发布固定 FAP1 PCM
记录。Binder 发现和 FMQ 重连不会进入 AudioFlinger 写入路径。

FloralDevice 不存在或处理落后时，AudioFlinger 时钟继续推进，超出边界的音频块
会被丢弃。下一条成功发布的记录携带不连续标志，使 Opus 会话重置预测状态。

麦克风 PCM 从 `/ipc/floral_stream/mic.sock` 进入 FloralDevice，再通过独立的
`IAudioPcmSource/default` 会话，以单声道 FMI1 记录送入四块容量的 FMQ。
HAL 接收线程维护四块本地队列，每次开始录音都获取新的 FMQ。输入读取丢弃
超过 40 ms 的旧记录，欠载时按采样节拍补静音，并在静音切换和 standby 时
清理缓冲。输入协议见
`packages/services/FloralDevice/docs/protocols/FMI1.md`。

## 产品集成

将本仓库映射到 `hardware/floral/audio` 并继承 `audio.mk`。产品打包
`audio.primary.floral`，并通过以下属性选择模块：

```text
ro.hardware.audio.primary=floral
```

Floral 产品片段统一负责已安装的完整策略文件集合。它将平台共享策略文件与
Floral 扬声器与麦克风 primary policy 一并复制，使每个 vendor 目标路径只有
一个 Make 所有者，同时保持标准 `audio_policy_configuration.xml` 引用关系
不变。

容器内部 FMQ 记录见 [FAP1 契约](protocols/FAP1.zh-CN.md)。FSA1 宿主协议位于
`packages/services/FloralDevice/docs/protocols/FSA1.md`。

## 安全边界

HAL 以物理扬声器相同的角色接收 AudioFlinger 最终主混音，不使用
MediaProjection、AudioPlaybackCapture 或录音 API。当前能力契约不承诺 DRM
及受保护音频行为，必须单独验证。

麦克风录音遵循标准 Android 录音权限、AppOps 和音频策略。宿主负责提供 PCM
音源；HAL 不采集宿主麦克风，也不承诺硬件预处理或真实硬件采集时间戳。
