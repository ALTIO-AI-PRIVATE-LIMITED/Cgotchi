#pragma once

#include <atomic>
#include <cstdint>

#include "audio_codec_if.h"
#include "device/contracts/microphone.hpp"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/audio/codec_i2c_control.hpp"
#include "platform/audio/i2s_codec_audio_sink.hpp"

namespace micropixel::platform::audio {

struct Es7210MicrophoneConfig final {
    const char* name{};
    const char* log_tag{};
    int i2c_port{};
    // Seven-bit address as required by i2c_master_bus_add_device().
    uint8_t i2c_address{};
    uint32_t i2c_clock_hz{100000U};
    // Must match the clock owner's I2S rate.
    uint32_t sample_rate{16000U};
    uint8_t default_gain_db{30U};
};

// ES7210 ADC as an I2S slave on the RX half of a full-duplex controller whose
// TX half (the speaker codec sink) drives MCLK/BCLK/WS. The ADC is set up on
// every Start() and powered down on Stop(); a dedicated task reads RX and
// hands the average of MIC1 and MIC2 to the consumer. While TX is stopped there is no clock and no samples arrive.
class Es7210Microphone final : public device::Microphone {
   public:
    Es7210Microphone(Es7210MicrophoneConfig config, I2sCodecAudioSink& clock_owner)
        : config_(config), clock_owner_(clock_owner) {}

    // Probes and attaches the ADC; the microphone stays unavailable on failure.
    [[nodiscard]] esp_err_t Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor);

    [[nodiscard]] uint32_t SampleRate() const override { return task_ != nullptr ? config_.sample_rate : 0U; }
    [[nodiscard]] int32_t Start(device::MicrophoneSampleSink sink, void* context, uint8_t gain_db) override;
    [[nodiscard]] int32_t Stop() override;

   private:
    // 10 ms at 16 kHz per read.
    static constexpr uint32_t kChunkFrames = 160U;

    static void TaskEntry(void* argument);
    void Run();
    [[nodiscard]] esp_err_t WriteRegisters(const uint8_t (*registers)[2], uint32_t count);
    [[nodiscard]] esp_err_t ConfigureCodec(uint8_t gain_db);
    [[nodiscard]] esp_err_t PowerDownCodec();

    Es7210MicrophoneConfig config_{};
    I2sCodecAudioSink& clock_owner_;
    CodecI2cControl control_{};
    i2c_master_dev_handle_t device_{};
    TaskHandle_t task_{};
    SemaphoreHandle_t idle_{};
    i2s_chan_handle_t rx_{};
    std::atomic<bool> running_{};
    device::MicrophoneSampleSink sink_{};
    void* sink_context_{};
    int16_t interleaved_[kChunkFrames * 2U]{};
    int16_t mono_[kChunkFrames]{};
};

}  // namespace micropixel::platform::audio
