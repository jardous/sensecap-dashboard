#include "internal.hpp"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "driver/ledc.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace dash::platform {
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static NetworkState state;
static Settings settings;
static QueueHandle_t save_queue, publish_queue;
static esp_mqtt_client_handle_t mqtt;
static esp_netif_t* station;
static bool led_ready = false;
static bool connecting = false; // guarded by lock (Wi-Fi event task + service task)
struct Telemetry { std::array<Reading, metric_count> readings; uint64_t now; };
uint64_t now_ms() { return esp_timer_get_time() / 1000; }
void network_snapshot(NetworkState& out) {
    portENTER_CRITICAL(&lock); out = state; portEXIT_CRITICAL(&lock);
}
bool wifi_connected() {
    portENTER_CRITICAL(&lock); const bool up = state.connected; portEXIT_CRITICAL(&lock); return up;
}
void update_host(size_t index, bool online, float latency, uint64_t now) {
    portENTER_CRITICAL(&lock);
    auto& h = state.hosts[index];
    h.checked = true; h.online = online; h.latency_ms = online ? latency : NAN;
    if (online) h.last_ok_ms = now;
    h.history[h.cursor] = h.latency_ms;
    h.cursor = (h.cursor + 1) % h.history.size();
    if (h.count < h.history.size()) ++h.count;
    portEXIT_CRITICAL(&lock);
}
static void wifi_event(void*, esp_event_base_t base, int32_t event, void*) {
    if (base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&lock); state.connected = false; connecting = false; portEXIT_CRITICAL(&lock);
        ESP_LOGW("wifi", "Disconnected; reconnect scheduled");
    } else if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        portENTER_CRITICAL(&lock); state.connected = true; portEXIT_CRITICAL(&lock);
        ESP_LOGI("wifi", "Acquired local address");
    }
}
static void mqtt_event(void*, esp_event_base_t, int32_t event, void*) {
    if (event == MQTT_EVENT_CONNECTED || event == MQTT_EVENT_DISCONNECTED) {
        const bool up = event == MQTT_EVENT_CONNECTED;
        portENTER_CRITICAL(&lock); state.mqtt_connected = up; portEXIT_CRITICAL(&lock);
        if (up) {
            char topic[96]; std::snprintf(topic, sizeof(topic), "sensecap/%s/availability", settings.device_id);
            esp_mqtt_client_enqueue(mqtt, topic, "online", 0, 1, true, true);
        }
    } else if (event == MQTT_EVENT_ERROR) ESP_LOGW("mqtt", "Connection error; retrying");
}
void apply_backlight(uint8_t percent) {
    if (!led_ready) {
        ledc_timer_config_t timer{};
        timer.speed_mode = LEDC_LOW_SPEED_MODE; timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.timer_num = LEDC_TIMER_0; timer.freq_hz = 5000; timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer));
        ledc_channel_config_t channel{};
        channel.gpio_num = 45; channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = LEDC_CHANNEL_0; channel.timer_sel = LEDC_TIMER_0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel)); led_ready = true;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
bool save_settings(const Settings& s) {
    return validate_settings(s) && xQueueSend(save_queue, &s, 0) == pdTRUE;
}
void publish(const ViewModel& model) {
    Telemetry t{model.readings, now_ms()}; xQueueOverwrite(publish_queue, &t);
}
static void send_telemetry(const Telemetry& t) {
    if (!mqtt) return;
    NetworkState net; network_snapshot(net);
    if (!net.mqtt_connected) return;
    char values[4][24];
    for (size_t i = 0; i < 4; ++i) {
        if (t.readings[i].fresh(t.now, settings.stale_ms))
            std::snprintf(values[i], sizeof(values[i]), "%.2f", double(t.readings[i].value));
        else std::strcpy(values[i], "null");
    }
    const bool th = t.readings[0].fresh(t.now, settings.stale_ms) && t.readings[1].fresh(t.now, settings.stale_ms);
    char dew[24] = "null", absolute[24] = "null";
    if (th) {
        const float d = dew_point(t.readings[0].value, t.readings[1].value);
        if (std::isfinite(d)) std::snprintf(dew, sizeof(dew), "%.2f", double(d));
        std::snprintf(absolute, sizeof(absolute), "%.2f", double(absolute_humidity(t.readings[0].value, t.readings[1].value)));
    }
    char payload[512], topic[96];
    std::snprintf(payload, sizeof(payload),
        "{\"schema\":1,\"uptime_s\":%llu,\"temperature\":%s,\"humidity\":%s,\"co2\":%s,\"voc_index\":%s,\"dew_point\":%s,\"absolute_humidity\":%s}",
        (unsigned long long)(t.now / 1000), values[0], values[1], values[2], values[3], dew, absolute);
    std::snprintf(topic, sizeof(topic), "sensecap/%s/state", settings.device_id);
    // QoS 0, non-retained and not stored offline: no stale retained measurements/outbox growth.
    if (esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 0, false, false) < 0)
        ESP_LOGW("mqtt", "Publish enqueue failed");
}
static void service(void*) {
    uint64_t retry_at = 0;
    unsigned failures = 0;
    bool ntp_started = false;
    for (;;) {
        Settings candidate;
        if (xQueueReceive(save_queue, &candidate, 0) == pdTRUE) {
            nvs_handle_t handle;
            esp_err_t err = nvs_open("dashboard", NVS_READWRITE, &handle);
            if (err == ESP_OK) {
                err = nvs_set_blob(handle, "settings", &candidate, sizeof(candidate));
                if (err == ESP_OK) err = nvs_commit(handle);
                nvs_close(handle);
            }
            if (err == ESP_OK) { ESP_LOGI("config", "Saved; rebooting"); vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }
            else ESP_LOGE("config", "Save failed: %s", esp_err_to_name(err));
        }
        if (settings.ssid[0] && !wifi_connected() && now_ms() >= retry_at) {
            // Connect is asynchronous; bounded exponential backoff, no event-loop sleeps.
            portENTER_CRITICAL(&lock); const bool was_connecting = connecting; connecting = !connecting; portEXIT_CRITICAL(&lock);
            if (!was_connecting) esp_wifi_connect();
            else esp_wifi_disconnect();
            retry_at = now_ms() + (1000u << (failures < 5 ? failures++ : 5));
        }
        if (wifi_connected()) {
            failures = 0;
            portENTER_CRITICAL(&lock); connecting = false; portEXIT_CRITICAL(&lock);
            if (!ntp_started && settings.ntp_server[0]) {
                esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                esp_sntp_setservername(0, settings.ntp_server);
                esp_sntp_init(); ntp_started = true;
            }
        }
        NetworkState current; network_snapshot(current);
        current.uptime_ms = now_ms(); current.free_heap = esp_get_free_heap_size();
        if (current.connected) {
            wifi_ap_record_t ap{};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) current.rssi = ap.rssi;
            esp_netif_ip_info_t ip{}; esp_netif_get_ip_info(station, &ip);
            std::snprintf(current.ip, sizeof(current.ip), IPSTR, IP2STR(&ip.ip));
            std::snprintf(current.gateway, sizeof(current.gateway), IPSTR, IP2STR(&ip.gw));
            esp_netif_dns_info_t dns{}; esp_netif_get_dns_info(station, ESP_NETIF_DNS_MAIN, &dns);
            std::snprintf(current.dns, sizeof(current.dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        } else { std::strcpy(current.ip, "--"); std::strcpy(current.gateway, "--"); std::strcpy(current.dns, "--"); }
        portENTER_CRITICAL(&lock);
        // Preserve host and event updates made since the snapshot.
        state.uptime_ms = current.uptime_ms; state.free_heap = current.free_heap; state.rssi = current.rssi;
        std::memcpy(state.ip, current.ip, sizeof(state.ip));
        std::memcpy(state.gateway, current.gateway, sizeof(state.gateway));
        std::memcpy(state.dns, current.dns, sizeof(state.dns));
        portEXIT_CRITICAL(&lock);
        Telemetry t;
        if (xQueueReceive(publish_queue, &t, 0) == pdTRUE) send_telemetry(t);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
void start(const Settings& s) {
    settings = s;
    save_queue = xQueueCreate(1, sizeof(Settings)); publish_queue = xQueueCreate(1, sizeof(Telemetry));
    configASSERT(save_queue && publish_queue);
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    station = esp_netif_create_default_wifi_sta(); configASSERT(station);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, s.ssid, sizeof(config.sta.ssid));
    std::memcpy(config.sta.password, s.wifi_password, sizeof(config.sta.password));
    config.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    std::snprintf(state.ssid, sizeof(state.ssid), "%s", s.ssid[0] ? s.ssid : "Not configured");
    if (s.mqtt_uri[0]) {
        static char will_topic[96];
        std::snprintf(will_topic, sizeof(will_topic), "sensecap/%s/availability", s.device_id);
        esp_mqtt_client_config_t c{};
        c.broker.address.uri = settings.mqtt_uri;
        c.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        c.credentials.username = settings.mqtt_user[0] ? settings.mqtt_user : nullptr;
        c.credentials.authentication.password = settings.mqtt_password[0] ? settings.mqtt_password : nullptr;
        c.credentials.client_id = settings.device_id;
        c.session.last_will.topic = will_topic; c.session.last_will.msg = "offline";
        c.session.last_will.qos = 1; c.session.last_will.retain = true;
        c.buffer.size = 1024; c.outbox.limit = 4096;
        mqtt = esp_mqtt_client_init(&c); configASSERT(mqtt);
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqtt_event, nullptr));
        ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
    }
    start_sensors(); start_hosts(settings);
    configASSERT(xTaskCreate(service, "net_services", 8192, nullptr, 3, nullptr) == pdPASS);
}
}
