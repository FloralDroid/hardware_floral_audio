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

#include <gtest/gtest.h>

#include <array>

namespace floral::audio {
namespace {

namespace pcm = device::audio;
constexpr int64_t kNowNs = 1'000'000'000;

pcm::AudioInputRecord Block(uint64_t firstFrame, int16_t sample) {
    pcm::AudioInputRecord record;
    record.generation = 1;
    record.sequence = firstFrame / pcm::kAudioInputFramesPerRecord + 1;
    record.first_frame_position = firstFrame;
    record.capture_timestamp_ns = kNowNs - pcm::kAudioInputRecordDurationNs;
    record.samples.fill(sample);
    return record;
}

TEST(AudioInputBufferTest, ReadsPartialPacketsAcrossRecordBoundaries) {
    AudioInputBuffer buffer;
    ASSERT_TRUE(buffer.Push(Block(0, 123), kNowNs, 0, false));
    ASSERT_TRUE(buffer.Push(Block(240, 456), kNowNs, 0, false));
    std::array<int16_t, 300> samples;
    ASSERT_EQ(buffer.Read(samples.data(), 100, kNowNs), 100u);
    EXPECT_EQ(samples[99], 123);
    ASSERT_EQ(buffer.Read(samples.data(), samples.size(), kNowNs), 300u);
    EXPECT_EQ(samples[139], 123);
    EXPECT_EQ(samples[140], 456);
    ASSERT_EQ(buffer.Read(samples.data(), samples.size(), kNowNs), 80u);
    EXPECT_EQ(samples[79], 456);
    EXPECT_EQ(buffer.Read(samples.data(), samples.size(), kNowNs), 0u);
}

TEST(AudioInputBufferTest, DropsOldestFramesAndResetsLostCounter) {
    AudioInputBuffer buffer;
    for (size_t index = 0; index <= pcm::kAudioInputQueueCapacity; ++index) {
        ASSERT_TRUE(buffer.Push(Block(index * 240, index), kNowNs, 0, false));
    }
    std::array<int16_t, 240> samples;
    ASSERT_EQ(buffer.Read(samples.data(), samples.size(), kNowNs), samples.size());
    EXPECT_EQ(samples.front(), 1);
    EXPECT_EQ(buffer.TakeLostFrames(), 240u);
    EXPECT_EQ(buffer.TakeLostFrames(), 0u);
    EXPECT_EQ(buffer.Read(samples.data(), samples.size(), kNowNs + pcm::kAudioInputMaxAgeNs), 0u);
    EXPECT_EQ(buffer.TakeLostFrames(), 720u);
}

TEST(AudioInputBufferTest, RejectsOldMuteDataAndClearsEarlierGenerations) {
    AudioInputBuffer buffer;
    ASSERT_TRUE(buffer.Push(Block(0, 111), kNowNs, 0, false));
    buffer.Clear();
    ASSERT_TRUE(buffer.Push(Block(240, 222), kNowNs, 0, true));
    ASSERT_TRUE(buffer.Push(Block(480, 333), kNowNs, kNowNs, false));
    auto fresh = Block(720, 444);
    fresh.capture_timestamp_ns = kNowNs;
    ASSERT_TRUE(buffer.Push(fresh, kNowNs, kNowNs, false));
    std::array<int16_t, 240> samples;
    ASSERT_EQ(buffer.Read(samples.data(), samples.size(), kNowNs), samples.size());
    EXPECT_EQ(samples.front(), 444);

    ASSERT_TRUE(buffer.Push(Block(960, 555), kNowNs, 0, false));
    auto next = Block(0, 666);
    next.generation = 2;
    ASSERT_TRUE(buffer.Push(next, kNowNs, 0, false));
    ASSERT_EQ(buffer.Read(samples.data(), samples.size(), kNowNs), samples.size());
    EXPECT_EQ(samples.front(), 666);
    EXPECT_FALSE(buffer.Push(next, kNowNs, 0, false));
    next.capture_timestamp_ns = kNowNs + 1;
    EXPECT_FALSE(buffer.Push(next, kNowNs, 0, false));
    EXPECT_EQ(buffer.GetStats().malformed_records, 2u);
}

}  // namespace
}  // namespace floral::audio
