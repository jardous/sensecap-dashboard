#include "usb_setup.hpp"
#include "app/platform.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dash::config {
namespace {
sniffer::ClientSettings baseline;
void worker(void*) {
    sniffer::SetupLine line;
    uint64_t last_request=0;
    ESP_LOGI("usb_setup","DASH_USB_LINK_READY");
    for (;;) {
        uint8_t bytes[64];
        const int count=uart_read_bytes(UART_NUM_0,bytes,sizeof(bytes),pdMS_TO_TICKS(20));
        for (int i=0;i<count;++i) {
            const auto result=line.feed(bytes[i]);
            if (result==sniffer::SetupLine::Result::More) continue;
            if (result==sniffer::SetupLine::Result::Discarded) {
                ESP_LOGW("usb_setup","DASH_USB_LINK_INVALID"); line.clear(); continue;
            }
            if (sniffer::is_link_setup_probe(line.data(),line.size())) ESP_LOGI("usb_setup","DASH_USB_LINK_READY");
            else {
                auto candidate=baseline;
                const auto now=platform::now_ms();
                if (!sniffer::decode_link_setup(line.data(),line.size(),candidate)) ESP_LOGW("usb_setup","DASH_USB_LINK_INVALID");
                else if (last_request && now-last_request<5000) ESP_LOGW("usb_setup","DASH_USB_LINK_BUSY");
                else {
                    last_request=now;
                    if (platform::provision_sniffer_link(candidate)) ESP_LOGI("usb_setup","DASH_USB_LINK_QUEUED");
                    else ESP_LOGW("usb_setup","DASH_USB_LINK_BUSY");
                }
                candidate={};
            }
            line.clear(); // never echo input, passwords or tokens
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
}
void start_usb_setup(const sniffer::ClientSettings& s) {
    baseline=s;
    // UART0 is the D1S CH340 console; RP2040 sensors use UART2. Buffer RX so
    // a full JSON request survives ordinary display/network scheduling delays.
    if (!uart_is_driver_installed(UART_NUM_0)) {
        if (uart_driver_install(UART_NUM_0,2048,0,0,nullptr,0)!=ESP_OK) {
            ESP_LOGE("usb_setup","USB setup unavailable; UART driver initialization failed"); return;
        }
    }
    configASSERT(xTaskCreate(worker,"usb_link_setup",6144,nullptr,2,nullptr)==pdPASS);
}
}
