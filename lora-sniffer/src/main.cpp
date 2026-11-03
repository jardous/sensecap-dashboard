#include "sniffer.hpp"
#include "serial_diagnostics.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

namespace receiver {
bool provision_valid(const Provision& p) {
    return sniffer::valid(p);
}
}
extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init()); // fail closed, never erase existing user NVS automatically
    std::snprintf(receiver::boot_id,sizeof(receiver::boot_id),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
    receiver::Provision provision;
    nvs_handle_t handle;
    if (nvs_open("sniffer",NVS_READONLY,&handle)==ESP_OK) {
        size_t size=sizeof(provision);
        if (nvs_get_blob(handle,"provision",&provision,&size)!=ESP_OK || size!=sizeof(provision) || !receiver::provision_valid(provision)) provision={};
        nvs_close(handle);
    }
    receiver::radio_start();
    if (receiver::provision_valid(provision)) receiver::network_start(provision);
    else ESP_LOGW("provision","Wi-Fi disabled. Provision over USB; no default credentials or open access point.");
    receiver::serial_console(); // buffered USB diagnostics + existing private setup
}
