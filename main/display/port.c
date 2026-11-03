/* SPDX-License-Identifier: Apache-2.0
 * Adapted from Seeed indicator_basis/main/lv_port.c, pinned in vendor.lock.
 * Retains BSP double framebuffer and dirty-region synchronization.
 * All LVGL calls except lv_tick_inc are owned by app_main.
 */
#include "port.h"
#include "lvgl.h"
#include "bsp_board.h"
#include "bsp_lcd.h"
#include "indev/indev.h"
#include "esp_timer.h"
#include "rom/cache.h"
#include <string.h>
static lv_disp_drv_t driver;
static display_performance_t performance;
display_performance_t display_take_performance(void) {
    display_performance_t result = performance;
    performance = (display_performance_t){0};
    return result;
}
static bool flush_done(void* arg) { lv_disp_flush_ready(&driver); return false; }
static bool flush_last(void) { return lv_disp_flush_is_last(&driver); }
static void flush(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* colors) {
    int64_t begin = esp_timer_get_time();
    ESP_ERROR_CHECK(bsp_lcd_flush(a->x1, a->y1, a->x2 + 1, a->y2 + 1, colors));
    uint32_t duration = esp_timer_get_time() - begin;
    if (duration > performance.max_flush_us) performance.max_flush_us = duration;
}
static void copy_dirty(void) {
    lv_disp_t* d = _lv_refr_get_disp_refreshing();
    uint8_t* from = d->driver->draw_buf->buf_act;
    uint8_t* to = from == d->driver->draw_buf->buf1 ? d->driver->draw_buf->buf2 : d->driver->draw_buf->buf1;
    const int stride = d->driver->hor_res * sizeof(lv_color_t);
    for (int i = 0; i < d->inv_p; ++i) {
        if (d->inv_area_joined[i]) continue;
        const lv_area_t* a = &d->inv_areas[i];
        const int bytes = (a->x2 - a->x1 + 1) * sizeof(lv_color_t);
        for (int y = a->y1; y <= a->y2; ++y) {
            const int offset = y * stride + a->x1 * sizeof(lv_color_t);
            memcpy(to + offset, from + offset, bytes);
        }
        Cache_WriteBack_Addr((uint32_t)(to + a->y1 * stride), (a->y2 - a->y1 + 1) * stride);
    }
}
static void touch(lv_indev_drv_t* d, lv_indev_data_t* out) {
    static lv_point_t last;
    static bool was_pressed = false;
    int64_t begin = esp_timer_get_time();
    indev_data_t in = {0};
    out->state = LV_INDEV_STATE_RELEASED;
    if (indev_get_major_value(&in) == ESP_OK && in.pressed) {
        // Matches Seeed basis rotation-0 mapping; clamp to valid [0,479].
        last.x = LV_CLAMP(0, 479 - in.x, 479);
        last.y = LV_CLAMP(0, 479 - in.y, 479);
        out->state = LV_INDEV_STATE_PRESSED;
    }
    out->point = last;
    const bool pressed = out->state == LV_INDEV_STATE_PRESSED;
    if (pressed != was_pressed) {
        was_pressed = pressed;
        // Request prompt visual feedback, never render recursively in input.
        lv_timer_ready(d->disp->refr_timer);
    }
    uint32_t duration = esp_timer_get_time() - begin;
    if (duration > performance.max_touch_us) performance.max_touch_us = duration;
}
static void tick(void* arg) { lv_tick_inc(2); }
void display_init(void) {
    ESP_ERROR_CHECK(bsp_board_init());
    lv_init();
    void *one = NULL, *two = NULL;
    bsp_lcd_get_frame_buffer(&one, &two);
    assert(one && two);
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, one, two, 480 * 480);
    lv_disp_drv_init(&driver);
    driver.hor_res = 480; driver.ver_res = 480;
    driver.draw_buf = &buffer; driver.flush_cb = flush; driver.direct_mode = 1;
    ESP_ERROR_CHECK(bsp_lcd_set_cb(flush_done, NULL));
    bsp_lcd_flush_is_last_register(flush_last); bsp_lcd_direct_mode_register(copy_dirty);
    lv_disp_drv_register(&driver);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input); input.type = LV_INDEV_TYPE_POINTER; input.read_cb = touch;
    lv_indev_drv_register(&input);
    lv_timer_set_period(input.read_timer, 10);
    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {.callback = tick, .name = "lv_tick", .skip_unhandled_events = true};
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 2000));
}
