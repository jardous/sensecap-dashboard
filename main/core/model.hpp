#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace dash {
enum class Metric : uint8_t { Temperature, Humidity, Co2, Voc, Count };
constexpr size_t metric_count = 4;
struct Reading {
    float value = NAN;
    uint64_t at_ms = 0;
    bool fresh(uint64_t now, uint32_t stale_ms) const {
        return std::isfinite(value) && now >= at_ms && now - at_ms <= stale_ms;
    }
};
struct Sample { Metric metric{}; float value = NAN; uint64_t at_ms = 0; };
bool valid(Metric metric, float value);
float dew_point(float temperature, float rh);
float absolute_humidity(float temperature, float rh);
enum class Health { Unknown, Healthy, Warning, Critical };
struct Thresholds {
    float co2_warn = 1000, co2_critical = 1500;
    float voc_warn = 150, voc_critical = 250;
    float temp_low = 18, temp_high = 27, rh_low = 30, rh_high = 65;
};
Health air_quality(const std::array<Reading, metric_count>& values,
                   uint64_t now, uint32_t stale_ms, const Thresholds& limits);

// PacketSerial/COBS: fixed bounded frame, 0 delimiter, type + LE IEEE754 float.
class SensorParser {
public:
    bool feed(uint8_t byte, uint64_t now, Sample& out);
    uint32_t rejected() const { return rejected_; }
private:
    std::array<uint8_t, 32> frame_{};
    size_t used_ = 0;
    bool discard_ = false;
    uint32_t rejected_ = 0;
};

// One-minute buckets retain 24h. Separate sum/count for each metric, no fake zeros.
class History {
public:
    static constexpr size_t capacity = 1440;
    void add(const Sample& sample);
    void chart(Metric metric, uint64_t now, uint32_t window_minutes,
               float* output, size_t count) const;
private:
    struct Bucket {
        uint64_t minute_plus_one = 0;
        std::array<float, metric_count> sum{};
        std::array<uint32_t, metric_count> count{};
    };
    std::array<Bucket, capacity> buckets_{};
};
enum class Check : uint8_t { Tcp, Http };
struct HostConfig {
    char name[32] = "Router";
    char address[128] = "";
    uint16_t port = 80;
    Check check = Check::Tcp;
};
struct HostState {
    bool checked = false, online = false;
    float latency_ms = NAN;
    uint64_t last_ok_ms = 0;
    std::array<float, 32> history{};
    uint8_t cursor = 0, count = 0;
};
struct NetworkState {
    bool connected = false;
    char ssid[33] = "Not configured", ip[16] = "--", gateway[16] = "--", dns[16] = "--";
    int rssi = 0;
    uint32_t free_heap = 0;
    uint64_t uptime_ms = 0;
    bool mqtt_connected = false;
    std::array<HostState, 4> hosts{};
};
struct Settings {
    uint32_t version = 1;
    char ssid[33] = "", wifi_password[65] = "";
    char mqtt_uri[160] = "", mqtt_user[64] = "", mqtt_password[96] = "";
    char device_id[32] = "sensecap";
    uint32_t publish_seconds = 30, refresh_ms = 1000, stale_ms = 20000;
    uint16_t screen_timeout_s = 120, rotation_s = 0;
    uint8_t brightness = 85;
    Thresholds limits{};
    std::array<HostConfig, 4> hosts{};
    char reticulum_url[160] = "", reticulum_token[96] = "";
    char lora_topic[128] = "";
    char ntp_server[128] = ""; // optional local NTP; no public internet dependency
};
bool validate_settings(const Settings& settings);
struct ViewModel {
    std::array<Reading, metric_count> readings{};
    History history;
    NetworkState network;
    Settings settings;
    bool simulated = false;
    uint32_t dropped_samples = 0;
};
} // namespace dash
