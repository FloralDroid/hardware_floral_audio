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

#include "floral/audio/AudioInputBuffer.h"

#include <algorithm>
#include <limits>

namespace floral::audio {
namespace pcm = device::audio;

void AudioInputBuffer::Lose(uint64_t frames) {
    stats_.dropped_frames += frames;
    lost_frames_ += frames;
}

void AudioInputBuffer::Clear() {
    for (const auto& packet : packets_) {
        Lose(pcm::kAudioInputFramesPerRecord - packet.offset);
    }
    packets_.clear();
    generation_ = 0;
    next_frame_ = 0;
}

bool AudioInputBuffer::Push(const pcm::AudioInputRecord& record, int64_t now_ns,
                            int64_t minimum_time_ns, bool muted) {
    if (record.generation == 0 || record.capture_timestamp_ns > static_cast<uint64_t>(now_ns) ||
        record.first_frame_position >
                std::numeric_limits<uint64_t>::max() - pcm::kAudioInputFramesPerRecord) {
        ++stats_.malformed_records;
        return false;
    }
    if (record.generation != generation_) {
        Clear();
        generation_ = record.generation;
        next_frame_ = record.first_frame_position;
    }
    if (record.first_frame_position < next_frame_) {
        ++stats_.malformed_records;
        return false;
    }
    Lose(record.first_frame_position - next_frame_);
    next_frame_ = record.first_frame_position + pcm::kAudioInputFramesPerRecord;
    if (muted || record.capture_timestamp_ns < static_cast<uint64_t>(minimum_time_ns) ||
        now_ns - static_cast<int64_t>(record.capture_timestamp_ns) > pcm::kAudioInputMaxAgeNs) {
        if (!muted) {
            Lose(pcm::kAudioInputFramesPerRecord);
        }
        return true;
    }
    if (packets_.size() == pcm::kAudioInputQueueCapacity) {
        Lose(pcm::kAudioInputFramesPerRecord - packets_.front().offset);
        packets_.pop_front();
    }
    packets_.push_back({record, 0});
    ++stats_.received_packets;
    return true;
}

size_t AudioInputBuffer::Read(int16_t* samples, size_t frames, int64_t now_ns) {
    size_t copied = 0;
    while (copied < frames && !packets_.empty()) {
        auto& packet = packets_.front();
        if (now_ns - static_cast<int64_t>(packet.record.capture_timestamp_ns) >
            pcm::kAudioInputMaxAgeNs) {
            Lose(pcm::kAudioInputFramesPerRecord - packet.offset);
            packets_.pop_front();
            continue;
        }
        const size_t count =
                std::min(frames - copied, pcm::kAudioInputFramesPerRecord - packet.offset);
        std::copy_n(packet.record.samples.data() + packet.offset, count, samples + copied);
        copied += count;
        packet.offset += count;
        if (packet.offset == pcm::kAudioInputFramesPerRecord) {
            packets_.pop_front();
        }
    }
    return copied;
}

uint32_t AudioInputBuffer::TakeLostFrames() {
    const uint32_t frames = std::min<uint64_t>(lost_frames_, std::numeric_limits<uint32_t>::max());
    lost_frames_ = 0;
    return frames;
}

AudioInputBufferStats AudioInputBuffer::GetStats() const {
    return stats_;
}

}  // namespace floral::audio
