#include "serial_diagnostics.hpp"
#include "sniffer.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_vfs_dev.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include <cstdio>

namespace receiver {
namespace {
void reply(const char* text) {
    // Share stdio's lock with ESP_LOG so diagnostics are whole JSON lines. IDF's
    // driver-backed VFS has a bounded TX wait if the host stops reading. Only
    // this low-priority console task performs the diagnostic USB writes.
    std::printf("\n%s\n",text);
}
void snapshot(bool with_radio) {
    // This scratch buffer is owned only by the console task. Keeping it in BSS
    // leaves headroom for cJSON/stdio on the 8KiB main stack.
    static char output[4096];
    auto state=network_diagnostics();
    std::snprintf(state.boot_id,sizeof(state.boot_id),"%s",boot_id);
    state.reset_reason=esp_reset_reason(); state.free_heap=esp_get_free_heap_size();
    state.console_stack_min_free=uxTaskGetStackHighWaterMark(nullptr);
    if (encode_network_diagnostics(state,now_ms(),output,sizeof(output))) reply(output);
    else reply("{\"schema\":1,\"type\":\"usb_error\",\"message\":\"Snapshot unavailable\"}");
    if (with_radio && sniffer::encode(radio_status(),output,sizeof(output))) reply(output);
}
void provision(const char* text,size_t n) {
    Provision next;
    nvs_handle_t handle;
    if (!sniffer::decode_provision(text,n,next) || !provision_valid(next)) {
        ESP_LOGW("provision","Invalid provision request"); return;
    }
    auto rc=nvs_open("sniffer",NVS_READWRITE,&handle);
    if (rc==ESP_OK) {
        rc=nvs_set_blob(handle,"provision",&next,sizeof(next));
        if (rc==ESP_OK) rc=nvs_commit(handle);
        nvs_close(handle);
    }
    next={};
    if (rc==ESP_OK) { ESP_LOGI("provision","Saved; rebooting"); vTaskDelay(pdMS_TO_TICKS(250)); esp_restart(); }
    else ESP_LOGE("provision","Save failed");
}
}
void serial_console() {
    usb_serial_jtag_driver_config_t config{}; config.rx_buffer_size=2048; config.tx_buffer_size=8192;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    esp_vfs_usb_serial_jtag_use_driver();
    setvbuf(stdout,nullptr,_IONBF,0);
    ESP_LOGI("usb_console","USB diagnostics ready: help / status / wifi / watch on / watch off");
    sniffer::SetupLine line;
    uint64_t watch_until=0,next_snapshot=0,last_reply=0;
    for (;;) {
        uint8_t bytes[64];
        const int n=usb_serial_jtag_read_bytes(bytes,sizeof(bytes),pdMS_TO_TICKS(20));
        for (int i=0;i<n;++i) {
            const auto result=line.feed(bytes[i]);
            if (result==sniffer::SetupLine::Result::More) continue;
            const auto now=now_ms();
            // Bound response work even under malformed/flooded input.
            if (last_reply && now-last_reply<100) { line.clear(); continue; }
            last_reply=now;
            const auto command=result==sniffer::SetupLine::Result::Complete?
                serial_command(line.data(),line.size()):SerialCommand::Invalid;
            switch (command) {
            case SerialCommand::Help:
                reply("{\"schema\":1,\"type\":\"usb_help\",\"commands\":[\"help\",\"status\",\"wifi\",\"watch on\",\"watch off\"],\"watch_lease_seconds\":60}"); break;
            case SerialCommand::Status: snapshot(true); break;
            case SerialCommand::Wifi: snapshot(false); break;
            case SerialCommand::WatchOn: watch_until=now+60000; next_snapshot=now; break;
            case SerialCommand::WatchOff:
                watch_until=0;
                reply("{\"schema\":1,\"type\":\"usb_watch\",\"enabled\":false}"); break;
            case SerialCommand::Provision: provision(line.data(),line.size()); break;
            default: reply("{\"schema\":1,\"type\":\"usb_error\",\"message\":\"Invalid command or line; use help\"}"); break;
            }
            line.clear(); // never echo arbitrary input, passwords or tokens
        }
        const auto now=now_ms();
        if (watch_until && now<watch_until && now>=next_snapshot) { snapshot(true); next_snapshot=now+1000; }
        if (watch_until && now>=watch_until) watch_until=0;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
}
