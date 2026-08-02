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

#define LOG_TAG "floral-audio-pcm"

#include "floral/audio/AudioPcmPublisher.h"

#include "floral/device/audio/AudioPcmRecord.h"

#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <aidl/floral/device/audio/AudioPcmStreamConfig.h>
#include <aidl/floral/device/audio/IAudioPcmSink.h>
#include <android/binder_manager.h>
#include <fmq/AidlMessageQueue.h>
#include <log/log.h>

#include <chrono>
#include <memory>
#include <utility>

namespace floral::audio {
namespace {

using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::floral::device::audio::AudioPcmStreamConfig;
using aidl::floral::device::audio::IAudioPcmSink;
using Fmq = android::AidlMessageQueue<int8_t, SynchronizedReadWrite>;

constexpr auto kQueueWait = std::chrono::milliseconds(100);
constexpr auto kReconnectDelay = std::chrono::milliseconds(250);
constexpr int64_t kFmqWriteTimeoutNs = 1'000'000;

struct PublisherConnection {
    std::shared_ptr<IAudioPcmSink> service;
    std::unique_ptr<Fmq> queue;
};

std::unique_ptr<PublisherConnection> Connect(const AudioPcmPublisherConfig& config) {
    ndk::SpAIBinder binder(AServiceManager_checkService(config.service_instance.c_str()));
    if (binder.get() == nullptr) {
        return nullptr;
    }

    std::shared_ptr<IAudioPcmSink> service = IAudioPcmSink::fromBinder(binder);
    if (service == nullptr) {
        return nullptr;
    }

    AudioPcmStreamConfig streamConfig;
    streamConfig.sampleRate = floral::device::audio::kAudioPcmSampleRate;
    streamConfig.channelCount = floral::device::audio::kAudioPcmChannelCount;
    streamConfig.sampleFormat = floral::device::audio::kAudioPcmSampleFormatS16Le;
    streamConfig.framesPerPacket = floral::device::audio::kAudioPcmFramesPerRecord;
    aidl::android::hardware::common::fmq::MQDescriptor<int8_t, SynchronizedReadWrite> descriptor;
    if (!service->openOutput(streamConfig, &descriptor).isOk()) {
        return nullptr;
    }

    auto queue = std::make_unique<Fmq>(descriptor, false);
    if (!queue->isValid()) {
        return nullptr;
    }
    auto connection = std::make_unique<PublisherConnection>();
    connection->service = std::move(service);
    connection->queue = std::move(queue);
    return connection;
}

}  // namespace

AudioPcmPublisher::AudioPcmPublisher(AudioPcmPublisherConfig config)
    : config_(std::move(config)), publisher_thread_(&AudioPcmPublisher::PublisherLoop, this) {}

AudioPcmPublisher::~AudioPcmPublisher() {
    running_.store(false);
    queue_.Stop();
    if (publisher_thread_.joinable()) {
        publisher_thread_.join();
    }
}

uint32_t AudioPcmPublisher::BeginStream() {
    return queue_.BeginStream();
}

void AudioPcmPublisher::EndStream() {
    queue_.EndStream();
}

bool AudioPcmPublisher::Enqueue(const int16_t* samples, size_t frame_count,
                                uint64_t first_frame_position, uint64_t presentation_timestamp_ns) {
    return queue_.Write(samples, frame_count, first_frame_position, presentation_timestamp_ns);
}

AudioPcmPublisherStats AudioPcmPublisher::GetStats() const {
    AudioPcmPublisherStats stats;
    stats.queue = queue_.GetStats();
    stats.published_packets = published_packets_.load();
    stats.connection_failures = connection_failures_.load();
    stats.fmq_write_failures = fmq_write_failures_.load();
    return stats;
}

void AudioPcmPublisher::PublisherLoop() {
    std::unique_ptr<PublisherConnection> connection;
    auto nextConnectAttempt = std::chrono::steady_clock::time_point::min();
    uint32_t deliveredGeneration = 0;
    uint64_t deliveredDiscontinuityEpoch = 0;

    while (running_.load()) {
        QueuedAudioPacket packet;
        if (!queue_.WaitAndPop(&packet, kQueueWait)) {
            continue;
        }

        if (connection != nullptr && !AIBinder_isAlive(connection->service->asBinder().get())) {
            connection.reset();
            queue_.MarkDiscontinuity();
        }
        if (connection == nullptr) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextConnectAttempt) {
                queue_.MarkDiscontinuity();
                continue;
            }
            connection = Connect(config_);
            if (connection == nullptr) {
                ++connection_failures_;
                nextConnectAttempt = now + kReconnectDelay;
                queue_.MarkDiscontinuity();
                continue;
            }
            deliveredGeneration = 0;
            deliveredDiscontinuityEpoch = 0;
            ALOGI("connected to %s", config_.service_instance.c_str());
        }

        floral::device::audio::AudioPcmRecord record;
        record.stream_generation = packet.generation;
        record.first_frame_position = packet.first_frame_position;
        record.presentation_timestamp_ns = packet.presentation_timestamp_ns;
        record.frame_count = packet.frame_count;
        record.samples = packet.samples;
        if (packet.generation != deliveredGeneration) {
            record.flags |= floral::device::audio::kAudioPcmRecordStreamStart;
        }
        if (packet.generation != deliveredGeneration ||
            packet.discontinuity_epoch != deliveredDiscontinuityEpoch) {
            record.flags |= floral::device::audio::kAudioPcmRecordDiscontinuity;
        }

        floral::device::audio::SerializedAudioPcmRecord serialized;
        std::string error;
        if (!floral::device::audio::SerializeAudioPcmRecord(record, &serialized, &error)) {
            ALOGE("failed to serialize PCM record: %s", error.c_str());
            queue_.MarkDiscontinuity();
            continue;
        }
        // This worker may wait briefly so the FMQ EventFlag wakes the service;
        // AudioFlinger's write thread remains isolated by the local queue.
        if (!connection->queue->writeBlocking(serialized.data(), serialized.size(),
                                              kFmqWriteTimeoutNs)) {
            ++fmq_write_failures_;
            queue_.MarkDiscontinuity();
            continue;
        }

        deliveredGeneration = packet.generation;
        deliveredDiscontinuityEpoch = packet.discontinuity_epoch;
        ++published_packets_;
    }
}

}  // namespace floral::audio
