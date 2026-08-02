# FAP1 PCM FMQ contract / FAP1 PCM FMQ 契约

[English](#english) | [中文](#中文)

## English

FAP1 is an internal fixed-record contract between `audio.primary.floral` and
`floral_device_service`. It is carried by the synchronized FMQ returned from
`floral.device.audio.IAudioPcmSink/openOutput`; it is not a Unix socket or a
host protocol.

Every record is exactly 1008 bytes: a 48-byte network-byte-order header followed
by 960 bytes of interleaved PCM S16_LE samples.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `FAP1` |
| 4 | 2 | Version, currently `1` |
| 6 | 2 | Header size, currently `48` |
| 8 | 4 | HAL stream generation |
| 12 | 4 | Record flags |
| 16 | 8 | First frame position since the output opened |
| 24 | 8 | First sample `CLOCK_MONOTONIC` presentation time in nanoseconds |
| 32 | 4 | Frame count, exactly `240` |
| 36 | 4 | PCM payload size, exactly `960` |
| 40 | 8 | Reserved, must be zero |
| 48 | 960 | 240 stereo PCM S16_LE frames |

Flag bit 0 marks a new HAL output generation. Bit 1 marks a discontinuity. The
consumer rejects records with unknown flags or a different fixed format. FMQ
capacity is eight records; the HAL's preceding local queue is bounded to four
records and never blocks AudioFlinger.

## 中文

FAP1 是 `audio.primary.floral` 与 `floral_device_service` 之间的容器内部固定
记录契约。它通过 `floral.device.audio.IAudioPcmSink/openOutput` 返回的同步 FMQ
传递，不是 Unix Socket，也不是宿主协议。

每条记录固定为 1008 字节：48 字节大端协议头，后接 960 字节按声道交错的 PCM
S16_LE 采样。

| 偏移 | 大小 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | 魔数 `FAP1` |
| 4 | 2 | 版本，当前为 `1` |
| 6 | 2 | 协议头大小，当前为 `48` |
| 8 | 4 | HAL 输出 generation |
| 12 | 4 | 记录标志 |
| 16 | 8 | 从输出打开后开始递增的首帧位置 |
| 24 | 8 | 首采样点 `CLOCK_MONOTONIC` 纳秒显示时间 |
| 32 | 4 | 帧数，固定为 `240` |
| 36 | 4 | PCM 负载大小，固定为 `960` |
| 40 | 8 | 保留字段，必须为零 |
| 48 | 960 | 240 帧双声道 PCM S16_LE |

标志 bit 0 表示新的 HAL 输出 generation，bit 1 表示不连续。消费者拒绝未知标志
或不符合固定格式的记录。FMQ 容量为八条记录；HAL 前置本地队列容量为四条，且
绝不阻塞 AudioFlinger。
