#include "core/model.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
namespace dash::platform {
static QueueHandle_t samples;
static void acquisition(void*) {
    SensorParser parser;
    std::array<bool, metric_count> seen{};
    uint8_t bytes[128];
    uint32_t rejected = 0, dropped = 0;
    for (;;) {
        const int n = uart_read_bytes(UART_NUM_2, bytes, sizeof(bytes), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; ++i) {
            Sample sample;
            if (parser.feed(bytes[i], esp_timer_get_time() / 1000, sample)) {
                if (!seen[size_t(sample.metric)]) {
                    seen[size_t(sample.metric)] = true;
                    ESP_LOGI("sensor", "First real UART sample metric=%u value=%.2f", unsigned(sample.metric), double(sample.value));
                }
                if (xQueueSend(samples, &sample, 0) != pdTRUE) ++dropped;
            }
        }
        if (parser.rejected() != rejected) {
            rejected = parser.rejected();
            ESP_LOGW("sensor", "Rejected UART packets: %lu", (unsigned long)rejected);
        }
        if (dropped) { ESP_LOGW("sensor", "Dropped %lu samples", (unsigned long)dropped); dropped = 0; }
    }
}
void start_sensors() {
    samples = xQueueCreate(32, sizeof(Sample));
    configASSERT(samples);
    uart_config_t config{};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 1024, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, 19, 20, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    configASSERT(xTaskCreate(acquisition, "rp2040_rx", 4096, nullptr, 4, nullptr) == pdPASS);
    // Do not issue undocumented commands; official v1.0.1 only handles shutdown.
}
bool next_sample(Sample& sample) { return xQueueReceive(samples, &sample, 0) == pdTRUE; }
}
