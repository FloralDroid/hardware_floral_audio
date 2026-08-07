# Floral Audio HAL

[简体中文](README.zh-CN.md)

`hardware_floral_audio` provides the primary physical speaker output for
FloralDroid. AudioFlinger mixes application and system playback into fixed
48 kHz, stereo, signed 16-bit PCM. The HAL publishes that final mix to
`floral_device_service` through the `floral.device.audio` stable AIDL and a
synchronized FMQ.

The HAL does not encode audio and does not open a host socket. FloralDevice
owns Opus encoding, dynamic bitrate control, FSA1 framing, and the host audio
socket.

## Current scope

- Legacy `audio.primary.floral` module wrapped by Android 12
  `android.hardware.audio@7.0-impl`.
- Output only; microphone input is intentionally unsupported.
- 48 kHz, stereo, PCM S16_LE.
- 240-frame blocks, representing 5 milliseconds of audio.
- Four-block bounded local queue followed by an eight-record synchronized FMQ.
- Oldest local-block drop and discontinuity signaling under backpressure.
- Monotonic frame position and first-sample `CLOCK_MONOTONIC` presentation time.

`out_write()` performs only virtual-device pacing and bounded memory copies. A
dedicated publisher thread discovers
`floral.device.audio.IAudioPcmSink/default`, opens the FMQ, and publishes fixed
FAP1 PCM records. Binder discovery and FMQ reconnection never run on
AudioFlinger's write path.

When FloralDevice is absent or falls behind, AudioFlinger timing continues and
bounded audio blocks are discarded. The next published record carries a
discontinuity marker so the Opus session resets its predictor state.

## Product integration

Map this repository to `hardware/floral/audio` and inherit `audio.mk`. It
packages `audio.primary.floral` and selects it with:

```text
ro.hardware.audio.primary=floral
```

The Floral product fragment owns the complete installed policy file set. It
copies the shared platform policy files together with Floral's output-only
48 kHz primary policy, so Make has one owner for each vendor path and the
standard `audio_policy_configuration.xml` include relationship remains intact.

The internal FMQ record is documented in the [FAP1 contract](protocols/FAP1.md).
The FSA1 host protocol belongs to
`packages/services/FloralDevice/docs/protocols/FSA1.md`.

## Security boundary

The HAL receives the final AudioFlinger primary mix in the same role as a
physical speaker. It does not use MediaProjection, AudioPlaybackCapture, or a
recording API. DRM and protected-audio behavior are not claimed by the current
contract and require separate validation.
