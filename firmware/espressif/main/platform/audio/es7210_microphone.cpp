#include "platform/audio/es7210_microphone.hpp"

#include <cstdlib>

#include <algorithm>
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include "freertos/idf_additions.h"
#endif
#include "work/task_policy.hpp"

namespace micropixel::platform::audio {
namespace {

constexpr uint32_t kTaskStackSize = 3072U;
constexpr uint32_t kReadTimeoutMs = 100U;
constexpr uint32_t kStopTimeoutMs = 500U;
constexpr uint8_t kKeepDefaultGain = 0xFFU;
// Peak levels are logged for this many seconds after each Start().
constexpr uint32_t kLevelLogSeconds = 3U;

}  // namespace

esp_err_t Es7210Microphone::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    if (bus == nullptr || device_ != nullptr || config_.i2c_address == 0U || config_.sample_rate == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t probe_status = ProbeCodec(executor, bus, config_.i2c_address);
    if (probe_status != ESP_OK) {
        ESP_LOGW(config_.log_tag, "%s did not acknowledge at I2C 0x%02x: %s", config_.name, config_.i2c_address,
                 esp_err_to_name(probe_status));
        return probe_status;
    }
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = config_.i2c_address;
    device_config.scl_speed_hz = config_.i2c_clock_hz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &device_), config_.log_tag,
                        "attach ADC control bus failed");
    control_.Bind(executor, device_, config_.i2c_address, config_.i2c_port);
    idle_ = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(idle_ != nullptr, ESP_ERR_NO_MEM, config_.log_tag, "create idle semaphore failed");
    TaskHandle_t task = nullptr;
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(TaskEntry, "micropixel_mic", kTaskStackSize, this,
                                                               task_policy::kAudioPriority, &task,
                                                               task_policy::kSystemCore,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t created = xTaskCreatePinnedToCore(TaskEntry, "micropixel_mic", kTaskStackSize, this,
                                                       task_policy::kAudioPriority, &task, task_policy::kSystemCore);
#endif
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, config_.log_tag, "create capture task failed");
    task_ = task;
    ESP_LOGI(config_.log_tag, "%s found at I2C 0x%02x", config_.name, config_.i2c_address);
    return ESP_OK;
}

esp_err_t Es7210Microphone::WriteRegisters(const uint8_t (*registers)[2], uint32_t count) {
    for (uint32_t index = 0U; index < count; ++index) {
        uint8_t value = registers[index][1];
        if (control_.interface.write_reg(control_.Interface(), registers[index][0], 1, &value, 1) !=
            ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t Es7210Microphone::ConfigureCodec(uint8_t gain_db) {
    // The Cheeko Gotchi hardware self-test sequence: slave mode, 16-bit
    // Philips I2S, MIC1 left and MIC2 right with their PGA and bias on,
    // MIC3/MIC4 (speaker loopback) powered down. Gain registers take
    // 0x10 (PGA on) | dB / 3.
    const uint8_t gain = static_cast<uint8_t>(0x10U | std::min<uint32_t>(gain_db / 3U, 11U));
    const uint8_t registers[][2] = {
        {0x00, 0xff}, {0x00, 0x41}, {0x01, 0x3f}, {0x09, 0x30}, {0x0a, 0x30}, {0x23, 0x2a}, {0x22, 0x0a},
        {0x20, 0x0a}, {0x21, 0x2a}, {0x08, 0x00}, {0x40, 0x43}, {0x41, 0x70}, {0x42, 0x70}, {0x07, 0x20},
        {0x02, 0xc1}, {0x11, 0x60}, {0x12, 0x00}, {0x01, 0x34}, {0x06, 0x00}, {0x47, 0x08}, {0x48, 0x08},
        {0x4b, 0x00}, {0x4c, 0xff}, {0x43, gain}, {0x44, gain}, {0x40, 0x43}, {0x00, 0x71}, {0x00, 0x41}};
    return WriteRegisters(registers, sizeof(registers) / sizeof(registers[0]));
}

esp_err_t Es7210Microphone::PowerDownCodec() {
    static constexpr uint8_t kRegisters[][2] = {{0x47, 0xff}, {0x48, 0xff}, {0x49, 0xff}, {0x4a, 0xff},
                                                {0x4b, 0xff}, {0x4c, 0xff}, {0x40, 0xc0}, {0x01, 0x7f},
                                                {0x06, 0x07}};
    return WriteRegisters(kRegisters, sizeof(kRegisters) / sizeof(kRegisters[0]));
}

int32_t Es7210Microphone::Start(device::MicrophoneSampleSink sink, void* context, uint8_t gain_db) {
    if (task_ == nullptr || sink == nullptr) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (running_.load(std::memory_order_acquire)) {
        return MICROPIXEL_STATUS_STALE_STATE;
    }
    rx_ = clock_owner_.InputChannel();
    if (rx_ == nullptr) {
        ESP_LOGW(config_.log_tag, "no full-duplex RX channel; is the speaker codec up?");
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    const uint8_t gain = gain_db == kKeepDefaultGain ? config_.default_gain_db : gain_db;
    if (ConfigureCodec(gain) != ESP_OK) {
        ESP_LOGE(config_.log_tag, "%s configuration failed", config_.name);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    const esp_err_t enable_status = i2s_channel_enable(rx_);
    if (enable_status != ESP_OK && enable_status != ESP_ERR_INVALID_STATE) {
        (void)PowerDownCodec();
        return MICROPIXEL_STATUS_INTERNAL;
    }
    sink_ = sink;
    sink_context_ = context;
    (void)xSemaphoreTake(idle_, 0U);
    running_.store(true, std::memory_order_release);
    xTaskNotifyGive(task_);
    ESP_LOGI(config_.log_tag, "capture on: %u Hz, MIC1+MIC2, gain %u dB", static_cast<unsigned>(config_.sample_rate),
             static_cast<unsigned>(gain));
    return MICROPIXEL_STATUS_OK;
}

int32_t Es7210Microphone::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return MICROPIXEL_STATUS_OK;
    }
    // The task finishes its current read (bounded by kReadTimeoutMs) before
    // the sink may go away.
    const bool idle = xSemaphoreTake(idle_, pdMS_TO_TICKS(kStopTimeoutMs)) == pdTRUE;
    (void)i2s_channel_disable(rx_);
    (void)PowerDownCodec();
    sink_ = nullptr;
    sink_context_ = nullptr;
    ESP_LOGI(config_.log_tag, "capture off");
    return idle ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_TIMEOUT;
}

void Es7210Microphone::TaskEntry(void* argument) { static_cast<Es7210Microphone*>(argument)->Run(); }

void Es7210Microphone::Run() {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t frames_since_log = 0U;
        uint32_t logged_seconds = 0U;
        int32_t peak[2]{};
        while (running_.load(std::memory_order_acquire)) {
            size_t bytes_read = 0U;
            const esp_err_t status =
                i2s_channel_read(rx_, interleaved_, sizeof(interleaved_), &bytes_read, pdMS_TO_TICKS(kReadTimeoutMs));
            const uint32_t frames = static_cast<uint32_t>(bytes_read / (2U * sizeof(int16_t)));
            if (status != ESP_OK && status != ESP_ERR_TIMEOUT) {
                ESP_LOGW(config_.log_tag, "RX read failed: %s", esp_err_to_name(status));
                vTaskDelay(pdMS_TO_TICKS(kReadTimeoutMs));
                continue;
            }
            if (frames == 0U || !running_.load(std::memory_order_acquire)) {
                continue;
            }
            for (uint32_t frame = 0U; frame < frames; ++frame) {
                // Both MEMS mics: averaging halves their uncorrelated noise.
                mono_[frame] = static_cast<int16_t>((static_cast<int32_t>(interleaved_[frame * 2U]) +
                                                     static_cast<int32_t>(interleaved_[frame * 2U + 1U])) /
                                                    2);
                for (uint32_t channel = 0U; channel < 2U; ++channel) {
                    const int32_t magnitude = std::abs(static_cast<int32_t>(interleaved_[frame * 2U + channel]));
                    if (magnitude > peak[channel]) {
                        peak[channel] = magnitude;
                    }
                }
            }
            sink_(sink_context_, mono_, frames);
            frames_since_log += frames;
            if (frames_since_log >= config_.sample_rate) {
                if (logged_seconds < kLevelLogSeconds) {
                    ESP_LOGI(config_.log_tag, "peak L=%ld R=%ld", static_cast<long>(peak[0]),
                             static_cast<long>(peak[1]));
                    ++logged_seconds;
                }
                frames_since_log = 0U;
                peak[0] = 0;
                peak[1] = 0;
            }
        }
        (void)xSemaphoreGive(idle_);
    }
}

}  // namespace micropixel::platform::audio
