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

#include <array>
#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

namespace floral::audio {
namespace {

using namespace std::chrono_literals;

TEST(AudioPacketQueueTest, PreservesPcmAndTimingMetadata) {
    AudioPacketQueue queue;
    const uint32_t generation = queue.BeginStream();
    std::array<int16_t, kFloralAudioSamplesPerPacket> samples{};
    for (size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<int16_t>(index);
    }

    ASSERT_TRUE(queue.Write(samples.data(), kFloralAudioPacketFrames, 960, 4'000'000));
    QueuedAudioPacket packet;
    ASSERT_TRUE(queue.WaitAndPop(&packet, 1ms));
    EXPECT_EQ(generation, packet.generation);
    EXPECT_EQ(kFloralAudioPacketFrames, packet.frame_count);
    EXPECT_EQ(960U, packet.first_frame_position);
    EXPECT_EQ(4'000'000U, packet.presentation_timestamp_ns);
    EXPECT_EQ(samples, packet.samples);
}

TEST(AudioPacketQueueTest, CombinesPartialWritesWithoutChangingFramePosition) {
    AudioPacketQueue queue;
    queue.BeginStream();
    std::array<int16_t, 200 * kFloralAudioChannelCount> first{};
    std::array<int16_t, 280 * kFloralAudioChannelCount> second{};
    first.fill(11);
    second.fill(22);

    ASSERT_TRUE(queue.Write(first.data(), 200, 100, 8'000'000));
    ASSERT_TRUE(queue.Write(second.data(), 280, 300, 12'166'666));
    QueuedAudioPacket packet;
    ASSERT_TRUE(queue.WaitAndPop(&packet, 1ms));
    EXPECT_EQ(100U, packet.first_frame_position);
    EXPECT_EQ(8'000'000U, packet.presentation_timestamp_ns);
    EXPECT_EQ(11, packet.samples[0]);
    EXPECT_EQ(11, packet.samples[first.size() - 1]);
    EXPECT_EQ(22, packet.samples[first.size()]);
    EXPECT_EQ(22, packet.samples.back());
}

TEST(AudioPacketQueueTest, DropsOldestPacketAndMarksDiscontinuityOnOverflow) {
    AudioPacketQueue queue;
    queue.BeginStream();
    std::array<int16_t, kFloralAudioSamplesPerPacket> samples{};

    for (size_t index = 0; index < kFloralAudioQueuePacketCapacity + 1; ++index) {
        samples.fill(static_cast<int16_t>(index));
        ASSERT_TRUE(queue.Write(samples.data(), kFloralAudioPacketFrames,
                                index * kFloralAudioPacketFrames, index * 5'000'000));
    }

    QueuedAudioPacket packet;
    ASSERT_TRUE(queue.WaitAndPop(&packet, 1ms));
    EXPECT_EQ(kFloralAudioPacketFrames, packet.first_frame_position);
    EXPECT_GE(packet.discontinuity_epoch, 2U);
    EXPECT_EQ(1U, queue.GetStats().dropped_packets);
}

TEST(AudioPacketQueueTest, NewStreamClearsOldPacketsAndChangesGeneration) {
    AudioPacketQueue queue;
    const uint32_t firstGeneration = queue.BeginStream();
    std::array<int16_t, kFloralAudioSamplesPerPacket> samples{};
    ASSERT_TRUE(queue.Write(samples.data(), kFloralAudioPacketFrames, 0, 1));

    const uint32_t secondGeneration = queue.BeginStream();
    ASSERT_NE(firstGeneration, secondGeneration);
    QueuedAudioPacket packet;
    EXPECT_FALSE(queue.WaitAndPop(&packet, 1ms));

    ASSERT_TRUE(queue.Write(samples.data(), kFloralAudioPacketFrames, 240, 5'000'001));
    ASSERT_TRUE(queue.WaitAndPop(&packet, 1ms));
    EXPECT_EQ(secondGeneration, packet.generation);
}

}  // namespace
}  // namespace floral::audio
