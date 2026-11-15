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
#include <memory>
#include <string>

namespace floral::audio {

struct AudioPcmReceiverStats {
    uint64_t received_packets = 0;
    uint64_t dropped_frames = 0;
    uint64_t underrun_frames = 0;
    uint64_t connection_failures = 0;
    uint64_t malformed_records = 0;
};

class AudioPcmReceiver {
  public:
    explicit AudioPcmReceiver(std::string service_instance);
    ~AudioPcmReceiver();

    AudioPcmReceiver(const AudioPcmReceiver&) = delete;
    AudioPcmReceiver& operator=(const AudioPcmReceiver&) = delete;

    void BeginStream();
    void EndStream();
    void SetMuted(bool muted);
    void Read(int16_t* samples, size_t frame_count, int64_t deadline_ns);
    uint32_t TakeLostFrames();
    AudioPcmReceiverStats GetStats() const;

  private:
    struct Impl;
    const std::unique_ptr<Impl> impl_;
};

}  // namespace floral::audio
