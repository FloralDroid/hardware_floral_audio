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

#define LOG_TAG "floral-microphone-reader"

#include "floral/audio/AudioPcmReceiver.h"

#include "floral/audio/AudioInputBuffer.h"

#include <aidl/floral/device/audio/IAudioPcmSource.h>
#include <android/binder_manager.h>
#include <fmq/AidlMessageQueue.h>
#include <log/log.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace floral::audio {
namespace {

namespace pcm = device::audio;
using aidl::floral::device::audio::IAudioPcmInputStream;
using aidl::floral::device::audio::IAudioPcmSource;
using Fmq = android::AidlMessageQueue<int8_t,
                                      aidl::android::hardware::common::fmq::SynchronizedReadWrite>;

int64_t MonotonicNanoseconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
}

struct Connection {
    ~Connection() {
        if (stream != nullptr) {
            stream->close();
        }
    }
    std::shared_ptr<IAudioPcmInputStream> stream;
    std::unique_ptr<Fmq> queue;
    uint64_t revision = 0;
    std::chrono::steady_clock::time_point last_health_check = std::chrono::steady_clock::now();
};

std::unique_ptr<Connection> Connect(const std::string& instance, uint64_t revision) {
    ndk::SpAIBinder binder(AServiceManager_checkService(instance.c_str()));
    if (binder.get() == nullptr) {
        return nullptr;
    }
    auto service = IAudioPcmSource::fromBinder(binder);
    if (service == nullptr) {
        return nullptr;
    }
    aidl::floral::device::audio::AudioPcmStreamConfig config;
    config.sampleRate = pcm::kAudioInputSampleRate;
    config.channelCount = pcm::kAudioInputChannelCount;
    config.sampleFormat = pcm::kAudioInputSampleFormatS16Le;
    config.framesPerPacket = pcm::kAudioInputFramesPerRecord;
    auto connection = std::make_unique<Connection>();
    if (!service->openInput(config, &connection->stream).isOk() || connection->stream == nullptr) {
        return nullptr;
    }
    aidl::android::hardware::common::fmq::MQDescriptor<
            int8_t, aidl::android::hardware::common::fmq::SynchronizedReadWrite>
            descriptor;
    if (!connection->stream->start(&descriptor).isOk()) {
        return nullptr;
    }
    connection->queue = std::make_unique<Fmq>(descriptor, false);
    if (!connection->queue->isValid() ||
        connection->queue->getQuantumCount() !=
                pcm::kAudioInputRecordSize * pcm::kAudioInputQueueCapacity) {
        return nullptr;
    }
    connection->revision = revision;
    return connection;
}

}  // namespace

struct AudioPcmReceiver::Impl {
    explicit Impl(std::string instance)
        : service_instance(std::move(instance)), worker(&Impl::Run, this) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            running = false;
        }
        condition.notify_all();
        worker.join();
    }

    void Run() {
        std::unique_ptr<Connection> connection;
        while (true) {
            uint64_t currentRevision;
            {
                std::unique_lock lock(mutex);
                if (!running) {
                    break;
                }
                if (!active) {
                    lock.unlock();
                    connection.reset();
                    lock.lock();
                    condition.wait(lock, [this]() { return !running || active; });
                    continue;
                }
                currentRevision = revision;
            }
            if (connection != nullptr &&
                (connection->revision != currentRevision ||
                 !AIBinder_isAlive(connection->stream->asBinder().get()))) {
                connection.reset();
                std::lock_guard lock(mutex);
                buffer.Clear();
            }
            if (connection == nullptr) {
                connection = Connect(service_instance, currentRevision);
                if (connection == nullptr) {
                    std::unique_lock lock(mutex);
                    ++stats.connection_failures;
                    // Bound service discovery while it is absent, and wake
                    // immediately for close or another recording generation.
                    condition.wait_for(
                            lock, std::chrono::milliseconds(250), [this, currentRevision]() {
                                return !running || !active || revision != currentRevision;
                            });
                    continue;
                }
            }

            pcm::SerializedAudioInputRecord bytes;
            if (!connection->queue->readBlocking(bytes.data(), bytes.size(),
                                                 pcm::kAudioInputRecordDurationNs)) {
                // An idle FMQ also outlives a dead producer. Probe Binder on
                // this worker so service restart does not leave capture stuck.
                const auto now = std::chrono::steady_clock::now();
                if (now - connection->last_health_check >= std::chrono::milliseconds(250)) {
                    connection->last_health_check = now;
                    if (AIBinder_ping(connection->stream->asBinder().get()) != STATUS_OK) {
                        connection.reset();
                        std::lock_guard lock(mutex);
                        buffer.Clear();
                    }
                }
                continue;
            }
            pcm::AudioInputRecord record;
            std::string error;
            if (!pcm::ParseAudioInputRecord(bytes, &record, &error)) {
                ALOGE("invalid microphone FMQ record: %s", error.c_str());
                connection.reset();
                std::lock_guard lock(mutex);
                ++stats.malformed_records;
                buffer.Clear();
                continue;
            }
            const int64_t nowNs = MonotonicNanoseconds();
            std::lock_guard lock(mutex);
            if (!active || revision != currentRevision) {
                continue;
            }
            if (!buffer.Push(record, nowNs, minimum_time_ns, muted)) {
                ALOGE("invalid microphone sample position or capture time");
                buffer.Clear();
            }
            condition.notify_all();
        }
    }

    const std::string service_instance;
    mutable std::mutex mutex;
    std::condition_variable condition;
    AudioInputBuffer buffer;
    AudioPcmReceiverStats stats;
    uint64_t revision = 0;
    int64_t minimum_time_ns = 0;
    bool active = false;
    bool muted = false;
    bool running = true;
    std::thread worker;
};

AudioPcmReceiver::AudioPcmReceiver(std::string service_instance)
    : impl_(std::make_unique<Impl>(std::move(service_instance))) {}
AudioPcmReceiver::~AudioPcmReceiver() = default;

void AudioPcmReceiver::BeginStream() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->buffer.Clear();
        impl_->active = true;
        ++impl_->revision;
        impl_->minimum_time_ns = MonotonicNanoseconds();
    }
    impl_->condition.notify_all();
}

void AudioPcmReceiver::EndStream() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->active = false;
        ++impl_->revision;
        impl_->buffer.Clear();
    }
    impl_->condition.notify_all();
}

void AudioPcmReceiver::SetMuted(bool muted) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->muted == muted) {
            return;
        }
        impl_->muted = muted;
        impl_->minimum_time_ns = MonotonicNanoseconds();
        impl_->buffer.Clear();
    }
    impl_->condition.notify_all();
}

void AudioPcmReceiver::Read(int16_t* samples, size_t frame_count, int64_t deadline_ns) {
    std::fill_n(samples, frame_count, 0);
    size_t copied = 0;
    std::unique_lock lock(impl_->mutex);
    const int64_t minimumTimeNs = impl_->minimum_time_ns;
    const uint64_t revision = impl_->revision;
    while (copied < frame_count && impl_->active && !impl_->muted && impl_->running) {
        const int64_t nowNs = MonotonicNanoseconds();
        const size_t count = impl_->buffer.Read(samples + copied, frame_count - copied, nowNs);
        copied += count;
        if (copied == frame_count || nowNs >= deadline_ns) {
            break;
        }
        impl_->condition.wait_for(lock, std::chrono::nanoseconds(deadline_ns - nowNs));
    }
    // Keep the virtual capture clock running even with a bursty host or a
    // muted microphone. Recheck the mute boundary after this wait.
    while (impl_->running && impl_->active) {
        const int64_t remainingNs = deadline_ns - MonotonicNanoseconds();
        if (remainingNs <= 0) {
            break;
        }
        impl_->condition.wait_for(lock, std::chrono::nanoseconds(remainingNs));
    }
    if (impl_->muted || minimumTimeNs != impl_->minimum_time_ns || revision != impl_->revision) {
        std::fill_n(samples, frame_count, 0);
    } else {
        impl_->stats.underrun_frames += frame_count - copied;
    }
}

uint32_t AudioPcmReceiver::TakeLostFrames() {
    std::lock_guard lock(impl_->mutex);
    return impl_->buffer.TakeLostFrames();
}

AudioPcmReceiverStats AudioPcmReceiver::GetStats() const {
    std::lock_guard lock(impl_->mutex);
    AudioPcmReceiverStats stats = impl_->stats;
    const auto bufferStats = impl_->buffer.GetStats();
    stats.received_packets = bufferStats.received_packets;
    stats.dropped_frames = bufferStats.dropped_frames;
    stats.malformed_records += bufferStats.malformed_records;
    return stats;
}

}  // namespace floral::audio
