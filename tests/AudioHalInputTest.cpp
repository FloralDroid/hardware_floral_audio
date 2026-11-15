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

#include <gtest/gtest.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <array>
#include <cerrno>
#include <chrono>

extern "C" audio_module HAL_MODULE_INFO_SYM;

namespace {

class AudioHalInputTest : public ::testing::Test {
  protected:
    void SetUp() override {
        hw_device_t* opened = nullptr;
        ASSERT_EQ(HAL_MODULE_INFO_SYM.common.methods->open(&HAL_MODULE_INFO_SYM.common,
                                                           AUDIO_HARDWARE_INTERFACE, &opened),
                  0);
        device = reinterpret_cast<audio_hw_device*>(opened);
    }

    void TearDown() override {
        if (input != nullptr) {
            device->close_input_stream(device, input);
        }
        if (output != nullptr) {
            device->close_output_stream(device, output);
        }
        if (device != nullptr) {
            device->common.close(&device->common);
        }
    }

    int OpenInput(audio_config* config, audio_stream_in** stream) {
        return device->open_input_stream(device, 1, AUDIO_DEVICE_IN_BUILTIN_MIC, config, stream,
                                         AUDIO_INPUT_FLAG_NONE, "", AUDIO_SOURCE_MIC);
    }

    audio_hw_device* device = nullptr;
    audio_stream_in* input = nullptr;
    audio_stream_out* output = nullptr;
};

TEST_F(AudioHalInputTest, NegotiatesMonoAndKeepsInputIndependentOfOutput) {
    audio_config config = AUDIO_CONFIG_INITIALIZER;
    config.sample_rate = 16000;
    EXPECT_EQ(OpenInput(&config, &input), -EINVAL);
    EXPECT_EQ(input, nullptr);
    EXPECT_EQ(config.sample_rate, 48000u);
    EXPECT_EQ(config.channel_mask, AUDIO_CHANNEL_IN_MONO);
    EXPECT_EQ(config.format, AUDIO_FORMAT_PCM_16_BIT);
    EXPECT_EQ(device->get_input_buffer_size(device, &config), 480u);
    ASSERT_EQ(OpenInput(&config, &input), 0);
    audio_stream_in* duplicate = nullptr;
    EXPECT_EQ(OpenInput(&config, &duplicate), -EBUSY);
    EXPECT_EQ(duplicate, nullptr);

    audio_config outputConfig = AUDIO_CONFIG_INITIALIZER;
    ASSERT_EQ(device->open_output_stream(device, 2, AUDIO_DEVICE_OUT_SPEAKER,
                                         AUDIO_OUTPUT_FLAG_PRIMARY, &outputConfig, &output, ""),
              0);
    EXPECT_EQ(outputConfig.channel_mask, AUDIO_CHANNEL_OUT_STEREO);
    EXPECT_EQ(input->common.get_channels(&input->common), AUDIO_CHANNEL_IN_MONO);
}

TEST_F(AudioHalInputTest, MuteIsPacedAndCapturePositionSurvivesStandby) {
    ASSERT_EQ(device->set_mic_mute(device, true), 0);
    audio_config config = AUDIO_CONFIG_INITIALIZER;
    ASSERT_EQ(OpenInput(&config, &input), 0);
    std::array<int16_t, 240> samples;
    samples.fill(1234);
    const auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(input->read(input, samples.data(), sizeof(samples)),
              static_cast<ssize_t>(sizeof(samples)));
    EXPECT_GE(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(4));
    for (int16_t sample : samples) {
        EXPECT_EQ(sample, 0);
    }
    int64_t frames = 0;
    int64_t time = 0;
    ASSERT_EQ(input->get_capture_position(input, &frames, &time), 0);
    EXPECT_EQ(frames, 240);
    EXPECT_GT(time, 0);
    ASSERT_EQ(input->common.standby(&input->common), 0);
    ASSERT_EQ(input->read(input, samples.data(), sizeof(samples)),
              static_cast<ssize_t>(sizeof(samples)));
    int64_t nextFrames = 0;
    int64_t nextTime = 0;
    ASSERT_EQ(input->get_capture_position(input, &nextFrames, &nextTime), 0);
    EXPECT_EQ(nextFrames, 480);
    EXPECT_GE(nextTime, time);
    EXPECT_EQ(input->read(input, samples.data(), 1), -EINVAL);
    EXPECT_EQ(input->get_capture_position(input, nullptr, &time), -EINVAL);
}

}  // namespace
