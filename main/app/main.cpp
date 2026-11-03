#include "platform.hpp"
#include "display/port.h"
#include "ui/dashboard.hpp"
#include "lora/client.hpp"
#include "config/usb_setup.hpp"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
static dash::ViewModel model; // history lives in BSS, not on task stack
extern "C" void app_main() {
    // Keep the UI above background service tasks; display sync still yields.
    vTaskPrioritySet(nullptr, 5);
    model.settings = dash::platform::load_settings();
    display_init();
    dash::platform::apply_backlight(model.settings.brightness);
    dash::platform::start_screen_button();
    dash::platform::start(model.settings);
    dash::lora::start(model.settings.sniffer);
    dash::config::start_usb_setup(model.settings.sniffer);
    dash::ui::create(model);
    uint64_t rendered = 0, published = 0;
    uint64_t performance_at = 0;
    uint32_t handler_max_us = 0;
    for (;;) {
        dash::Sample sample;
        while (dash::platform::next_sample(sample)) {
            model.readings[size_t(sample.metric)] = {sample.value, sample.at_ms};
            model.history.add(sample);
        }
        const auto now = dash::platform::now_ms();
        if (dash::platform::take_screen_toggle()) dash::ui::toggle_screen();
        dash::lora::poll(model.lora);
        if (now - rendered >= model.settings.refresh_ms) {
            dash::platform::network_snapshot(model.network);
            dash::ui::refresh(now); rendered = now;
        }
        if (now - published >= model.settings.publish_seconds * 1000ull) {
            dash::platform::publish(model); published = now;
        }
        const auto begin = esp_timer_get_time();
        lv_timer_handler();
        const uint32_t handler_us = esp_timer_get_time() - begin;
        if (handler_us > handler_max_us) handler_max_us = handler_us;
        if (now - performance_at >= 10000 && now <= 60000) {
            const auto stats = display_take_performance();
            ESP_LOGI("ui_timing", "max handler=%lu us, flush=%lu us, touch read=%lu us",
                (unsigned long)handler_max_us, (unsigned long)stats.max_flush_us, (unsigned long)stats.max_touch_us);
            handler_max_us = 0; performance_at = now;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
