# FAP1 PCM FMQ Contract

[简体中文](FAP1.zh-CN.md)

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
