#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void display_init(void);
typedef struct {
    uint32_t max_flush_us;
    uint32_t max_touch_us;
} display_performance_t;
display_performance_t display_take_performance(void);
#ifdef __cplusplus
}
#endif
