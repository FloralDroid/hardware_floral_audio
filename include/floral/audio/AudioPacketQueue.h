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

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace floral::audio {

constexpr uint32_t kFloralAudioSampleRate = 48'000;
constexpr uint16_t kFloralAudioChannelCount = 2;
constexpr uint32_t kFloralAudioPacketFrames = 240;
constexpr size_t kFloralAudioSamplesPerPacket = kFloralAudioPacketFrames * kFloralAudioChannelCount;
constexpr size_t kFloralAudioQueuePacketCapacity = 4;

struct QueuedAudioPacket {
    std::array<int16_t, kFloralAudioSamplesPerPacket> samples{};
    uint32_t frame_count = 0;
    uint32_t generation = 0;
    uint64_t discontinuity_epoch = 0;
    uint64_t first_frame_position = 0;
    uint64_t presentation_timestamp_ns = 0;
};

struct AudioPacketQueueStats {
    uint64_t enqueued_packets = 0;
    uint64_t dequeued_packets = 0;
    uint64_t dropped_packets = 0;
};

class AudioPacketQueue {
  public:
    AudioPacketQueue() = default;
    AudioPacketQueue(const AudioPacketQueue&) = delete;
    AudioPacketQueue& operator=(const AudioPacketQueue&) = delete;

    uint32_t BeginStream();
    void EndStream();
    void Stop();

    bool Write(const int16_t* samples, size_t frameCount, uint64_t firstFramePosition,
               uint64_t presentationTimestampNs);
    bool WaitAndPop(QueuedAudioPacket* packet, std::chrono::milliseconds timeout);
    void MarkDiscontinuity();

    AudioPacketQueueStats GetStats() const;

  private:
    void ClearLocked();
    void EnqueuePendingLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::array<QueuedAudioPacket, kFloralAudioQueuePacketCapacity> packets_{};
    size_t head_ = 0;
    size_t packet_count_ = 0;

    std::array<int16_t, kFloralAudioSamplesPerPacket> pending_samples_{};
    size_t pending_frames_ = 0;
    uint64_t pending_first_frame_position_ = 0;
    uint64_t pending_presentation_timestamp_ns_ = 0;

    bool stream_active_ = false;
    bool stopped_ = false;
    uint32_t generation_ = 0;
    uint64_t discontinuity_epoch_ = 0;
    AudioPacketQueueStats stats_{};
};

}  // namespace floral::audio
