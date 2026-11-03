#include "app/platform.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
namespace dash::platform {
Settings load_settings() {
    Settings settings;
    const auto error = nvs_flash_init();
    if (error != ESP_OK) {
        ESP_LOGE("config", "NVS unavailable (%s); not erasing user storage", esp_err_to_name(error));
        return settings;
    }
    nvs_handle_t handle;
    if (nvs_open("dashboard", NVS_READONLY, &handle) != ESP_OK) return settings;
    Settings candidate;
    size_t size = sizeof(CoreSettings);
    const auto result = nvs_get_blob(handle, "settings", static_cast<CoreSettings*>(&candidate), &size);
    if (result == ESP_OK && size == sizeof(CoreSettings) && validate_settings(candidate)) settings = candidate;
    else if (result != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW("config", "Ignoring invalid/old settings");
    sniffer::ClientSettings lora;
    size = sizeof(lora);
    if (nvs_get_blob(handle, "lora_v1", &lora, &size) == ESP_OK && size == sizeof(lora) && sniffer::valid(lora)) settings.sniffer = lora;
    nvs_close(handle);
    return settings;
}
}
