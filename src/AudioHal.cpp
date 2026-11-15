/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio.primary.floral"

#include "floral/audio/AudioPacketQueue.h"
#include "floral/audio/AudioPcmPublisher.h"
#include "floral/audio/AudioPcmReceiver.h"

#include <aidl/floral/device/audio/IAudioPcmSink.h>
#include <aidl/floral/device/audio/IAudioPcmSource.h>

#include <errno.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <string.h>
#include <system/audio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>

namespace {

constexpr uint32_t kOutputLatencyMilliseconds = 5;
constexpr size_t kBytesPerFrame = floral::audio::kFloralAudioChannelCount * sizeof(int16_t);
constexpr int64_t kNanosecondsPerSecond = 1'000'000'000LL;
constexpr int64_t kUnderrunResetThresholdNs = 100'000'000LL;

int64_t MonotonicNanoseconds() {
    timespec timestamp{};
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return static_cast<int64_t>(timestamp.tv_sec) * kNanosecondsPerSecond + timestamp.tv_nsec;
}

uint64_t FramesToNanoseconds(uint64_t frames) {
    const uint64_t wholeSeconds = frames / floral::audio::kFloralAudioSampleRate;
    const uint64_t remainingFrames = frames % floral::audio::kFloralAudioSampleRate;
    return wholeSeconds * kNanosecondsPerSecond +
           remainingFrames * kNanosecondsPerSecond / floral::audio::kFloralAudioSampleRate;
}

void NanosecondsToTimespec(int64_t nanoseconds, timespec* timestamp) {
    timestamp->tv_sec = nanoseconds / kNanosecondsPerSecond;
    timestamp->tv_nsec = nanoseconds % kNanosecondsPerSecond;
}

void SleepUntil(int64_t deadlineNs) {
    timespec deadline{};
    NanosecondsToTimespec(deadlineNs, &deadline);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
    }
}

struct FloralStreamOut;
struct FloralStreamIn;
namespace input_pcm = floral::device::audio;

floral::audio::AudioPcmPublisherConfig MakeAudioPcmPublisherConfig() {
    floral::audio::AudioPcmPublisherConfig config;
    config.service_instance =
            std::string(aidl::floral::device::audio::IAudioPcmSink::descriptor) + "/default";
    return config;
}

struct FloralAudioDevice {
    audio_hw_device device{};
    mutable std::mutex mutex;
    FloralStreamOut* output = nullptr;
    FloralStreamIn* input = nullptr;
    bool mic_muted = false;
};

struct FloralStreamIn {
    FloralStreamIn()
        : receiver(std::string(aidl::floral::device::audio::IAudioPcmSource::descriptor) +
                   "/default") {}

    audio_stream_in stream{};
    mutable std::mutex mutex;
    floral::audio::AudioPcmReceiver receiver;
    bool standby = true;
    int64_t frames_read = 0;
    int64_t capture_time_ns = 0;
    int64_t next_read_deadline_ns = 0;
};

struct FloralStreamOut {
    FloralStreamOut() : publisher(MakeAudioPcmPublisherConfig()) {}

    audio_stream_out stream{};
    mutable std::mutex mutex;
    floral::audio::AudioPcmPublisher publisher;
    bool standby = true;
    uint64_t frames_written_total = 0;
    uint64_t presentation_base_frames = 0;
    uint64_t stream_frames_written = 0;
    int64_t stream_start_ns = 0;
    int64_t next_write_deadline_ns = 0;
    int64_t presentation_base_time_ns = 0;
};

FloralStreamOut* ToOutput(const audio_stream* stream) {
    return reinterpret_cast<FloralStreamOut*>(const_cast<audio_stream*>(stream));
}

const FloralStreamOut* ToOutput(const audio_stream_out* stream) {
    return reinterpret_cast<const FloralStreamOut*>(stream);
}

uint64_t PresentedFramesLocked(const FloralStreamOut& output, int64_t nowNs,
                               int64_t* presentationTimeNs) {
    if (output.standby) {
        *presentationTimeNs = output.presentation_base_time_ns;
        return output.presentation_base_frames;
    }

    const uint64_t elapsedFrames = nowNs <= output.stream_start_ns
                                           ? 0
                                           : static_cast<uint64_t>(nowNs - output.stream_start_ns) *
                                                     floral::audio::kFloralAudioSampleRate /
                                                     kNanosecondsPerSecond;
    const uint64_t activePresented = std::min(output.stream_frames_written, elapsedFrames);
    *presentationTimeNs = output.stream_start_ns + FramesToNanoseconds(activePresented);
    return output.presentation_base_frames + activePresented;
}

void BeginOutputStreamLocked(FloralStreamOut* output, int64_t nowNs) {
    output->standby = false;
    output->presentation_base_frames = output->frames_written_total;
    output->stream_frames_written = 0;
    output->stream_start_ns = nowNs;
    output->next_write_deadline_ns = nowNs;
    output->presentation_base_time_ns = nowNs;
    output->publisher.BeginStream();
}

uint32_t OutputGetSampleRate(const audio_stream* stream) {
    (void)stream;
    return floral::audio::kFloralAudioSampleRate;
}

int OutputSetSampleRate(audio_stream* stream, uint32_t rate) {
    (void)stream;
    return rate == floral::audio::kFloralAudioSampleRate ? 0 : -ENOSYS;
}

size_t OutputGetBufferSize(const audio_stream* stream) {
    (void)stream;
    return floral::audio::kFloralAudioPacketFrames * kBytesPerFrame;
}

audio_channel_mask_t OutputGetChannels(const audio_stream* stream) {
    (void)stream;
    return AUDIO_CHANNEL_OUT_STEREO;
}

audio_format_t OutputGetFormat(const audio_stream* stream) {
    (void)stream;
    return AUDIO_FORMAT_PCM_16_BIT;
}

int OutputSetFormat(audio_stream* stream, audio_format_t format) {
    (void)stream;
    return format == AUDIO_FORMAT_PCM_16_BIT ? 0 : -ENOSYS;
}

int OutputStandby(audio_stream* stream) {
    FloralStreamOut* output = ToOutput(stream);
    std::lock_guard lock(output->mutex);
    if (!output->standby) {
        output->presentation_base_frames = output->frames_written_total;
        output->presentation_base_time_ns = MonotonicNanoseconds();
        output->stream_frames_written = 0;
        output->standby = true;
        output->publisher.EndStream();
    }
    return 0;
}

int OutputDump(const audio_stream* stream, int fd) {
    const FloralStreamOut* output = ToOutput(reinterpret_cast<const audio_stream_out*>(stream));
    const floral::audio::AudioPcmPublisherStats stats = output->publisher.GetStats();
    dprintf(fd,
            "Floral primary audio:\n"
            "  format: PCM S16_LE, 48000 Hz, stereo\n"
            "  queued packets: %llu\n"
            "  FMQ packets: %llu\n"
            "  dropped packets: %llu\n"
            "  connection failures: %llu\n"
            "  FMQ write failures: %llu\n",
            static_cast<unsigned long long>(stats.queue.enqueued_packets),
            static_cast<unsigned long long>(stats.published_packets),
            static_cast<unsigned long long>(stats.queue.dropped_packets),
            static_cast<unsigned long long>(stats.connection_failures),
            static_cast<unsigned long long>(stats.fmq_write_failures));
    return 0;
}

int OutputSetParameters(audio_stream* stream, const char* keyValuePairs) {
    (void)stream;
    (void)keyValuePairs;
    return 0;
}

char* OutputGetParameters(const audio_stream* stream, const char* keys) {
    (void)stream;
    (void)keys;
    return strdup("");
}

int OutputAddAudioEffect(const audio_stream* stream, effect_handle_t effect) {
    (void)stream;
    (void)effect;
    return 0;
}

int OutputRemoveAudioEffect(const audio_stream* stream, effect_handle_t effect) {
    (void)stream;
    (void)effect;
    return 0;
}

uint32_t OutputGetLatency(const audio_stream_out* stream) {
    (void)stream;
    return kOutputLatencyMilliseconds;
}

int OutputSetVolume(audio_stream_out* stream, float left, float right) {
    (void)stream;
    (void)left;
    (void)right;
    return -ENOSYS;
}

ssize_t OutputWrite(audio_stream_out* stream, const void* buffer, size_t bytes) {
    if (buffer == nullptr || bytes == 0 || bytes % kBytesPerFrame != 0) {
        return -EINVAL;
    }

    auto* output = reinterpret_cast<FloralStreamOut*>(stream);
    const size_t frameCount = bytes / kBytesPerFrame;
    std::lock_guard lock(output->mutex);

    int64_t nowNs = MonotonicNanoseconds();
    if (output->standby) {
        BeginOutputStreamLocked(output, nowNs);
    } else if (nowNs > output->next_write_deadline_ns + kUnderrunResetThresholdNs) {
        // A long producer gap starts a new generation so the host can discard stale
        // timing.
        output->publisher.EndStream();
        BeginOutputStreamLocked(output, nowNs);
    }

    if (output->next_write_deadline_ns > nowNs) {
        SleepUntil(output->next_write_deadline_ns);
        nowNs = MonotonicNanoseconds();
    }

    const uint64_t firstFramePosition = output->frames_written_total;
    const uint64_t presentationTimestampNs =
            output->stream_start_ns + FramesToNanoseconds(output->stream_frames_written);
    output->publisher.Enqueue(static_cast<const int16_t*>(buffer), frameCount, firstFramePosition,
                              presentationTimestampNs);

    output->frames_written_total += frameCount;
    output->stream_frames_written += frameCount;
    const int64_t scheduledBase = std::max(output->next_write_deadline_ns, nowNs);
    output->next_write_deadline_ns = scheduledBase + FramesToNanoseconds(frameCount);
    return bytes;
}

int OutputGetRenderPosition(const audio_stream_out* stream, uint32_t* dspFrames) {
    if (dspFrames == nullptr) {
        return -EINVAL;
    }

    const auto* output = ToOutput(stream);
    std::lock_guard lock(output->mutex);
    int64_t presentationTimeNs = 0;
    *dspFrames = static_cast<uint32_t>(
            PresentedFramesLocked(*output, MonotonicNanoseconds(), &presentationTimeNs));
    return 0;
}

int OutputGetNextWriteTimestamp(const audio_stream_out* stream, int64_t* timestamp) {
    (void)stream;
    if (timestamp != nullptr) {
        *timestamp = 0;
    }
    return -EINVAL;
}

int OutputGetPresentationPosition(const audio_stream_out* stream, uint64_t* frames,
                                  timespec* timestamp) {
    if (frames == nullptr || timestamp == nullptr) {
        return -EINVAL;
    }

    const auto* output = ToOutput(stream);
    std::lock_guard lock(output->mutex);
    if (output->frames_written_total == 0) {
        return -ENODATA;
    }

    int64_t presentationTimeNs = 0;
    *frames = PresentedFramesLocked(*output, MonotonicNanoseconds(), &presentationTimeNs);
    NanosecondsToTimespec(presentationTimeNs, timestamp);
    return 0;
}

FloralStreamIn* ToInput(const audio_stream* stream) {
    return reinterpret_cast<FloralStreamIn*>(const_cast<audio_stream*>(stream));
}

uint32_t InputGetSampleRate(const audio_stream*) {
    return input_pcm::kAudioInputSampleRate;
}

int InputSetSampleRate(audio_stream*, uint32_t rate) {
    return rate == input_pcm::kAudioInputSampleRate ? 0 : -ENOSYS;
}

size_t InputGetBufferSize(const audio_stream*) {
    return input_pcm::kAudioInputPayloadSize;
}

audio_channel_mask_t InputGetChannels(const audio_stream*) {
    return AUDIO_CHANNEL_IN_MONO;
}

audio_format_t InputGetFormat(const audio_stream*) {
    return AUDIO_FORMAT_PCM_16_BIT;
}

int InputSetFormat(audio_stream*, audio_format_t format) {
    return format == AUDIO_FORMAT_PCM_16_BIT ? 0 : -ENOSYS;
}

audio_devices_t InputGetDevice(const audio_stream*) {
    return AUDIO_DEVICE_IN_BUILTIN_MIC;
}

int InputStandby(audio_stream* stream) {
    auto* input = ToInput(stream);
    std::lock_guard lock(input->mutex);
    input->standby = true;
    input->receiver.EndStream();
    return 0;
}

int InputDump(const audio_stream* stream, int fd) {
    const auto stats = ToInput(stream)->receiver.GetStats();
    dprintf(fd,
            "Floral microphone input: PCM S16_LE, 48000 Hz, mono\n"
            "  received packets: %llu\n"
            "  dropped frames: %llu\n"
            "  underrun frames: %llu\n"
            "  connection failures: %llu\n"
            "  malformed records: %llu\n",
            static_cast<unsigned long long>(stats.received_packets),
            static_cast<unsigned long long>(stats.dropped_frames),
            static_cast<unsigned long long>(stats.underrun_frames),
            static_cast<unsigned long long>(stats.connection_failures),
            static_cast<unsigned long long>(stats.malformed_records));
    return 0;
}

int InputSetParameters(audio_stream*, const char*) {
    return 0;
}

char* InputGetParameters(const audio_stream*, const char*) {
    return strdup("");
}

int InputAddAudioEffect(const audio_stream*, effect_handle_t) {
    return -ENOSYS;
}

int InputRemoveAudioEffect(const audio_stream*, effect_handle_t) {
    return -ENOSYS;
}

int InputSetGain(audio_stream_in*, float) {
    return -ENOSYS;
}

ssize_t InputRead(audio_stream_in* stream, void* buffer, size_t bytes) {
    if (buffer == nullptr || bytes == 0 || bytes % sizeof(int16_t) != 0 ||
        bytes > static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
        return -EINVAL;
    }
    auto* input = reinterpret_cast<FloralStreamIn*>(stream);
    const size_t frames = bytes / sizeof(int16_t);
    std::lock_guard lock(input->mutex);
    const int64_t nowNs = MonotonicNanoseconds();
    if (input->standby || nowNs > input->next_read_deadline_ns + kUnderrunResetThresholdNs) {
        input->receiver.BeginStream();
        input->standby = false;
        input->next_read_deadline_ns = nowNs;
    }
    input->next_read_deadline_ns =
            std::max(input->next_read_deadline_ns, nowNs) + FramesToNanoseconds(frames);
    input->receiver.Read(static_cast<int16_t*>(buffer), frames, input->next_read_deadline_ns);
    input->frames_read += frames;
    input->capture_time_ns = MonotonicNanoseconds();
    return bytes;
}

uint32_t InputGetFramesLost(audio_stream_in* stream) {
    return reinterpret_cast<FloralStreamIn*>(stream)->receiver.TakeLostFrames();
}

int InputGetCapturePosition(const audio_stream_in* stream, int64_t* frames, int64_t* time) {
    if (frames == nullptr || time == nullptr) {
        return -EINVAL;
    }
    const auto* input = reinterpret_cast<const FloralStreamIn*>(stream);
    std::lock_guard lock(input->mutex);
    if (input->capture_time_ns == 0) {
        return -ENOSYS;
    }
    *frames = input->frames_read;
    *time = input->capture_time_ns;
    return 0;
}

int DeviceInitCheck(const audio_hw_device* device) {
    (void)device;
    return 0;
}

int DeviceSetVoiceVolume(audio_hw_device* device, float volume) {
    (void)device;
    (void)volume;
    return -ENOSYS;
}

int DeviceSetMasterVolume(audio_hw_device* device, float volume) {
    (void)device;
    (void)volume;
    return -ENOSYS;
}

int DeviceGetMasterVolume(audio_hw_device* device, float* volume) {
    (void)device;
    (void)volume;
    return -ENOSYS;
}

int DeviceSetMasterMute(audio_hw_device* device, bool muted) {
    (void)device;
    (void)muted;
    return -ENOSYS;
}

int DeviceGetMasterMute(audio_hw_device* device, bool* muted) {
    (void)device;
    (void)muted;
    return -ENOSYS;
}

int DeviceSetMode(audio_hw_device* device, audio_mode_t mode) {
    (void)device;
    (void)mode;
    return 0;
}

int DeviceSetMicMute(audio_hw_device* device, bool state) {
    auto* floralDevice = reinterpret_cast<FloralAudioDevice*>(device);
    std::lock_guard lock(floralDevice->mutex);
    floralDevice->mic_muted = state;
    if (floralDevice->input != nullptr) {
        floralDevice->input->receiver.SetMuted(state);
    }
    return 0;
}

int DeviceGetMicMute(const audio_hw_device* device, bool* state) {
    if (state == nullptr) {
        return -EINVAL;
    }
    const auto* floralDevice = reinterpret_cast<const FloralAudioDevice*>(device);
    std::lock_guard lock(floralDevice->mutex);
    *state = floralDevice->mic_muted;
    return 0;
}

int DeviceSetParameters(audio_hw_device* device, const char* keyValuePairs) {
    (void)device;
    (void)keyValuePairs;
    return 0;
}

char* DeviceGetParameters(const audio_hw_device* device, const char* keys) {
    (void)device;
    (void)keys;
    return strdup("");
}

size_t DeviceGetInputBufferSize(const audio_hw_device* device, const audio_config* config) {
    (void)device;
    if (config == nullptr || config->sample_rate != input_pcm::kAudioInputSampleRate ||
        config->channel_mask != AUDIO_CHANNEL_IN_MONO ||
        config->format != AUDIO_FORMAT_PCM_16_BIT) {
        return 0;
    }
    return input_pcm::kAudioInputPayloadSize;
}

int DeviceOpenOutputStream(audio_hw_device* device, audio_io_handle_t handle,
                           audio_devices_t devices, audio_output_flags_t flags,
                           audio_config* config, audio_stream_out** streamOut,
                           const char* address) {
    (void)handle;
    (void)devices;
    (void)flags;
    (void)address;
    if (config == nullptr || streamOut == nullptr) {
        return -EINVAL;
    }
    *streamOut = nullptr;

    bool supported = true;
    if (config->sample_rate != 0 && config->sample_rate != floral::audio::kFloralAudioSampleRate) {
        supported = false;
    }
    if (config->channel_mask != AUDIO_CHANNEL_NONE &&
        config->channel_mask != AUDIO_CHANNEL_OUT_STEREO) {
        supported = false;
    }
    if (config->format != AUDIO_FORMAT_DEFAULT && config->format != AUDIO_FORMAT_PCM_16_BIT) {
        supported = false;
    }
    config->sample_rate = floral::audio::kFloralAudioSampleRate;
    config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config->format = AUDIO_FORMAT_PCM_16_BIT;
    if (!supported) {
        return -EINVAL;
    }

    auto* floralDevice = reinterpret_cast<FloralAudioDevice*>(device);
    std::lock_guard lock(floralDevice->mutex);
    if (floralDevice->output != nullptr) {
        return -EBUSY;
    }

    std::unique_ptr<FloralStreamOut> output(new (std::nothrow) FloralStreamOut());
    if (output == nullptr) {
        return -ENOMEM;
    }

    output->stream.common.get_sample_rate = OutputGetSampleRate;
    output->stream.common.set_sample_rate = OutputSetSampleRate;
    output->stream.common.get_buffer_size = OutputGetBufferSize;
    output->stream.common.get_channels = OutputGetChannels;
    output->stream.common.get_format = OutputGetFormat;
    output->stream.common.set_format = OutputSetFormat;
    output->stream.common.standby = OutputStandby;
    output->stream.common.dump = OutputDump;
    output->stream.common.set_parameters = OutputSetParameters;
    output->stream.common.get_parameters = OutputGetParameters;
    output->stream.common.add_audio_effect = OutputAddAudioEffect;
    output->stream.common.remove_audio_effect = OutputRemoveAudioEffect;
    output->stream.get_latency = OutputGetLatency;
    output->stream.set_volume = OutputSetVolume;
    output->stream.write = OutputWrite;
    output->stream.get_render_position = OutputGetRenderPosition;
    output->stream.get_next_write_timestamp = OutputGetNextWriteTimestamp;
    output->stream.get_presentation_position = OutputGetPresentationPosition;

    floralDevice->output = output.release();
    *streamOut = &floralDevice->output->stream;
    ALOGI("opened 48000 Hz stereo PCM output to FloralDevice FMQ");
    return 0;
}

void DeviceCloseOutputStream(audio_hw_device* device, audio_stream_out* stream) {
    auto* floralDevice = reinterpret_cast<FloralAudioDevice*>(device);
    std::unique_ptr<FloralStreamOut> output;
    {
        std::lock_guard lock(floralDevice->mutex);
        if (floralDevice->output == nullptr || &floralDevice->output->stream != stream) {
            return;
        }
        output.reset(floralDevice->output);
        floralDevice->output = nullptr;
    }
}

int DeviceOpenInputStream(audio_hw_device* device, audio_io_handle_t handle,
                          audio_devices_t devices, audio_config* config, audio_stream_in** streamIn,
                          audio_input_flags_t flags, const char* address, audio_source_t source) {
    (void)handle;
    (void)address;
    (void)source;
    if (streamIn == nullptr || config == nullptr) {
        return -EINVAL;
    }
    *streamIn = nullptr;
    const bool supported =
            (config->sample_rate == 0 || config->sample_rate == input_pcm::kAudioInputSampleRate) &&
            (config->channel_mask == AUDIO_CHANNEL_NONE ||
             config->channel_mask == AUDIO_CHANNEL_IN_MONO) &&
            (config->format == AUDIO_FORMAT_DEFAULT || config->format == AUDIO_FORMAT_PCM_16_BIT);
    config->sample_rate = input_pcm::kAudioInputSampleRate;
    config->channel_mask = AUDIO_CHANNEL_IN_MONO;
    config->format = AUDIO_FORMAT_PCM_16_BIT;
    if (!supported || devices != AUDIO_DEVICE_IN_BUILTIN_MIC || flags != AUDIO_INPUT_FLAG_NONE) {
        return -EINVAL;
    }

    auto* floralDevice = reinterpret_cast<FloralAudioDevice*>(device);
    std::lock_guard lock(floralDevice->mutex);
    if (floralDevice->input != nullptr) {
        return -EBUSY;
    }
    std::unique_ptr<FloralStreamIn> input(new (std::nothrow) FloralStreamIn());
    if (input == nullptr) {
        return -ENOMEM;
    }
    input->receiver.SetMuted(floralDevice->mic_muted);
    input->stream.common.get_sample_rate = InputGetSampleRate;
    input->stream.common.set_sample_rate = InputSetSampleRate;
    input->stream.common.get_buffer_size = InputGetBufferSize;
    input->stream.common.get_channels = InputGetChannels;
    input->stream.common.get_format = InputGetFormat;
    input->stream.common.set_format = InputSetFormat;
    input->stream.common.get_device = InputGetDevice;
    input->stream.common.standby = InputStandby;
    input->stream.common.dump = InputDump;
    input->stream.common.set_parameters = InputSetParameters;
    input->stream.common.get_parameters = InputGetParameters;
    input->stream.common.add_audio_effect = InputAddAudioEffect;
    input->stream.common.remove_audio_effect = InputRemoveAudioEffect;
    input->stream.set_gain = InputSetGain;
    input->stream.read = InputRead;
    input->stream.get_input_frames_lost = InputGetFramesLost;
    input->stream.get_capture_position = InputGetCapturePosition;
    floralDevice->input = input.release();
    *streamIn = &floralDevice->input->stream;
    ALOGI("opened 48000 Hz mono PCM input from FloralDevice FMQ");
    return 0;
}

void DeviceCloseInputStream(audio_hw_device* device, audio_stream_in* stream) {
    auto* floralDevice = reinterpret_cast<FloralAudioDevice*>(device);
    std::unique_ptr<FloralStreamIn> input;
    {
        std::lock_guard lock(floralDevice->mutex);
        if (floralDevice->input == nullptr || &floralDevice->input->stream != stream) {
            return;
        }
        input.reset(floralDevice->input);
        floralDevice->input = nullptr;
    }
}

int DeviceDump(const audio_hw_device* device, int fd) {
    const auto* floralDevice = reinterpret_cast<const FloralAudioDevice*>(device);
    std::lock_guard lock(floralDevice->mutex);
    if (floralDevice->output == nullptr) {
        dprintf(fd, "Floral primary audio: output closed\n");
    } else {
        OutputDump(&floralDevice->output->stream.common, fd);
    }
    if (floralDevice->input != nullptr) {
        InputDump(&floralDevice->input->stream.common, fd);
    }
    return 0;
}

int CloseDevice(hw_device_t* device) {
    auto* floralDevice = reinterpret_cast<FloralAudioDevice*>(device);
    {
        std::lock_guard lock(floralDevice->mutex);
        if (floralDevice->output != nullptr) {
            ALOGW("closing audio device with an open output stream");
            delete floralDevice->output;
            floralDevice->output = nullptr;
        }
        if (floralDevice->input != nullptr) {
            ALOGW("closing audio device with an open input stream");
            delete floralDevice->input;
            floralDevice->input = nullptr;
        }
    }
    delete floralDevice;
    return 0;
}

int OpenDevice(const hw_module_t* module, const char* name, hw_device_t** device) {
    if (name == nullptr || device == nullptr || strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        return -EINVAL;
    }

    std::unique_ptr<FloralAudioDevice> floralDevice(new (std::nothrow) FloralAudioDevice());
    if (floralDevice == nullptr) {
        return -ENOMEM;
    }

    floralDevice->device.common.tag = HARDWARE_DEVICE_TAG;
    floralDevice->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    floralDevice->device.common.module = const_cast<hw_module_t*>(module);
    floralDevice->device.common.close = CloseDevice;
    floralDevice->device.init_check = DeviceInitCheck;
    floralDevice->device.set_voice_volume = DeviceSetVoiceVolume;
    floralDevice->device.set_master_volume = DeviceSetMasterVolume;
    floralDevice->device.get_master_volume = DeviceGetMasterVolume;
    floralDevice->device.set_master_mute = DeviceSetMasterMute;
    floralDevice->device.get_master_mute = DeviceGetMasterMute;
    floralDevice->device.set_mode = DeviceSetMode;
    floralDevice->device.set_mic_mute = DeviceSetMicMute;
    floralDevice->device.get_mic_mute = DeviceGetMicMute;
    floralDevice->device.set_parameters = DeviceSetParameters;
    floralDevice->device.get_parameters = DeviceGetParameters;
    floralDevice->device.get_input_buffer_size = DeviceGetInputBufferSize;
    floralDevice->device.open_output_stream = DeviceOpenOutputStream;
    floralDevice->device.close_output_stream = DeviceCloseOutputStream;
    floralDevice->device.open_input_stream = DeviceOpenInputStream;
    floralDevice->device.close_input_stream = DeviceCloseInputStream;
    floralDevice->device.dump = DeviceDump;

    *device = &floralDevice.release()->device.common;
    return 0;
}

hw_module_methods_t kModuleMethods = {
        /* open */ OpenDevice,
};

}  // namespace

extern "C" {

audio_module HAL_MODULE_INFO_SYM = {
        /* common */ {
                /* tag */ HARDWARE_MODULE_TAG,
                /* module_api_version */ AUDIO_MODULE_API_VERSION_0_1,
                /* hal_api_version */ HARDWARE_HAL_API_VERSION,
                /* id */ AUDIO_HARDWARE_MODULE_ID,
                /* name */ "Floral primary audio HAL",
                /* author */ "FloralDroid",
                /* methods */ &kModuleMethods,
                /* dso */ nullptr,
                /* reserved */ {0},
        },
};

}  // extern "C"
