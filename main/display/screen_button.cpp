#include "app/platform.hpp"
#include "bsp_board.h"
#include "bsp_btn.h"
#include "esp_log.h"
#include <atomic>

namespace dash::platform {
namespace {
std::atomic<uint32_t> presses{0};
void pressed(void*) {
    // BSP debounces the physical GPIO38 button. This timer-task callback must
    // never touch LVGL, the backlight, NVS, Wi-Fi or sensor power directly.
    presses.fetch_add(1,std::memory_order_relaxed);
}
}
void start_screen_button() {
    ESP_ERROR_CHECK(bsp_btn_register_callback(BOARD_BTN_ID_USER,BUTTON_PRESS_DOWN,pressed,nullptr));
    ESP_LOGI("screen_ctrl","Physical user button controls backlight; idle blanking disabled");
}
bool take_screen_toggle() {
    // Preserve the parity of closely spaced presses without an unbounded queue.
    return presses.exchange(0,std::memory_order_relaxed)&1u;
}
}
