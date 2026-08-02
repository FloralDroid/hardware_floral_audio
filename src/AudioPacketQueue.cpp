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

#include "floral/audio/AudioPacketQueue.h"

#include <algorithm>

namespace floral::audio {
namespace {

constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

uint64_t FrameOffsetToNanoseconds(size_t frameOffset) {
    return static_cast<uint64_t>(frameOffset) * kNanosecondsPerSecond / kFloralAudioSampleRate;
}

}  // namespace

uint32_t AudioPacketQueue::BeginStream() {
    std::lock_guard lock(mutex_);
    ClearLocked();
    stream_active_ = true;
    ++generation_;
    if (generation_ == 0) {
        generation_ = 1;
    }
    ++discontinuity_epoch_;
    return generation_;
}

void AudioPacketQueue::EndStream() {
    std::lock_guard lock(mutex_);
    stream_active_ = false;
    ClearLocked();
    ++discontinuity_epoch_;
}

void AudioPacketQueue::Stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        stream_active_ = false;
        ClearLocked();
    }
    condition_.notify_all();
}

bool AudioPacketQueue::Write(const int16_t* samples, size_t frameCount, uint64_t firstFramePosition,
                             uint64_t presentationTimestampNs) {
    if (samples == nullptr || frameCount == 0) {
        return false;
    }

    std::lock_guard lock(mutex_);
    if (!stream_active_ || stopped_) {
        return false;
    }

    size_t consumedFrames = 0;
    while (consumedFrames < frameCount) {
        if (pending_frames_ == 0) {
            pending_first_frame_position_ = firstFramePosition + consumedFrames;
            pending_presentation_timestamp_ns_ =
                    presentationTimestampNs + FrameOffsetToNanoseconds(consumedFrames);
        }

        const size_t framesToCopy =
                std::min(kFloralAudioPacketFrames - pending_frames_, frameCount - consumedFrames);
        const size_t destinationSampleOffset = pending_frames_ * kFloralAudioChannelCount;
        const size_t sourceSampleOffset = consumedFrames * kFloralAudioChannelCount;
        std::copy_n(samples + sourceSampleOffset, framesToCopy * kFloralAudioChannelCount,
                    pending_samples_.begin() + destinationSampleOffset);
        pending_frames_ += framesToCopy;
        consumedFrames += framesToCopy;

        if (pending_frames_ == kFloralAudioPacketFrames) {
            EnqueuePendingLocked();
        }
    }

    condition_.notify_one();
    return true;
}

bool AudioPacketQueue::WaitAndPop(QueuedAudioPacket* packet, std::chrono::milliseconds timeout) {
    if (packet == nullptr) {
        return false;
    }

    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, timeout, [this]() { return stopped_ || packet_count_ != 0; });
    if (packet_count_ == 0) {
        return false;
    }

    *packet = packets_[head_];
    packet->discontinuity_epoch = discontinuity_epoch_;
    head_ = (head_ + 1) % packets_.size();
    --packet_count_;
    ++stats_.dequeued_packets;
    return true;
}

void AudioPacketQueue::MarkDiscontinuity() {
    std::lock_guard lock(mutex_);
    ++discontinuity_epoch_;
}

AudioPacketQueueStats AudioPacketQueue::GetStats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void AudioPacketQueue::ClearLocked() {
    head_ = 0;
    packet_count_ = 0;
    pending_frames_ = 0;
}

void AudioPacketQueue::EnqueuePendingLocked() {
    if (packet_count_ == packets_.size()) {
        head_ = (head_ + 1) % packets_.size();
        --packet_count_;
        ++stats_.dropped_packets;
        ++discontinuity_epoch_;
    }

    const size_t tail = (head_ + packet_count_) % packets_.size();
    QueuedAudioPacket& packet = packets_[tail];
    packet.samples = pending_samples_;
    packet.frame_count = pending_frames_;
    packet.generation = generation_;
    packet.discontinuity_epoch = discontinuity_epoch_;
    packet.first_frame_position = pending_first_frame_position_;
    packet.presentation_timestamp_ns = pending_presentation_timestamp_ns_;
    ++packet_count_;
    ++stats_.enqueued_packets;
    pending_frames_ = 0;
}

}  // namespace floral::audio
