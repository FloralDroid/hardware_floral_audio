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

#include "floral/device/audio/AudioInputRecord.h"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace floral::audio {

struct AudioInputBufferStats {
    uint64_t received_packets = 0;
    uint64_t dropped_frames = 0;
    uint64_t malformed_records = 0;
};

// The receiver serializes access; timestamps use guest CLOCK_MONOTONIC.
class AudioInputBuffer {
  public:
    void Clear();
    bool Push(const device::audio::AudioInputRecord& record, int64_t now_ns,
              int64_t minimum_time_ns, bool muted);
    size_t Read(int16_t* samples, size_t frames, int64_t now_ns);
    uint32_t TakeLostFrames();
    AudioInputBufferStats GetStats() const;

  private:
    struct Packet {
        device::audio::AudioInputRecord record;
        size_t offset = 0;
    };

    void Lose(uint64_t frames);

    std::deque<Packet> packets_;
    uint32_t generation_ = 0;
    uint64_t next_frame_ = 0;
    uint64_t lost_frames_ = 0;
    AudioInputBufferStats stats_;
};

}  // namespace floral::audio
