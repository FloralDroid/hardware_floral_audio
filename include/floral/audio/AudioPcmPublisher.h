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

#include "floral/audio/AudioPacketQueue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace floral::audio {

struct AudioPcmPublisherConfig {
    std::string service_instance;
};

struct AudioPcmPublisherStats {
    AudioPacketQueueStats queue;
    uint64_t published_packets = 0;
    uint64_t connection_failures = 0;
    uint64_t fmq_write_failures = 0;
};

// Keeps Binder discovery and FMQ publication outside AudioFlinger's write
// path. The local queue remains bounded while the service is unavailable.
class AudioPcmPublisher {
  public:
    explicit AudioPcmPublisher(AudioPcmPublisherConfig config);
    ~AudioPcmPublisher();

    AudioPcmPublisher(const AudioPcmPublisher&) = delete;
    AudioPcmPublisher& operator=(const AudioPcmPublisher&) = delete;

    uint32_t BeginStream();
    void EndStream();
    bool Enqueue(const int16_t* samples, size_t frame_count, uint64_t first_frame_position,
                 uint64_t presentation_timestamp_ns);
    AudioPcmPublisherStats GetStats() const;

  private:
    void PublisherLoop();

    const AudioPcmPublisherConfig config_;
    AudioPacketQueue queue_;
    std::atomic<bool> running_{true};
    std::atomic<uint64_t> published_packets_{0};
    std::atomic<uint64_t> connection_failures_{0};
    std::atomic<uint64_t> fmq_write_failures_{0};
    std::thread publisher_thread_;
};

}  // namespace floral::audio
