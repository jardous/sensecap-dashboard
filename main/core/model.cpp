#include "model.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace dash {
bool valid(Metric metric, float v) {
    if (!std::isfinite(v)) return false;
    switch (metric) {
        case Metric::Temperature: return v >= -40 && v <= 85;
        case Metric::Humidity: return v >= 0 && v <= 100;
        case Metric::Co2: return v >= 1 && v <= 40000;
        case Metric::Voc: return v >= 1 && v <= 500; // 0 = algorithm blackout
        default: return false;
    }
}
float dew_point(float t, float rh) {
    if (!valid(Metric::Temperature, t) || !valid(Metric::Humidity, rh) || rh == 0) return NAN;
    const float g = std::log(rh / 100.f) + 17.62f * t / (243.12f + t);
    return 243.12f * g / (17.62f - g);
}
float absolute_humidity(float t, float rh) {
    if (!valid(Metric::Temperature, t) || !valid(Metric::Humidity, rh)) return NAN;
    return 216.7f * (rh / 100.f * 6.112f * std::exp(17.67f * t / (t + 243.5f))) / (273.15f + t);
}
Health air_quality(const std::array<Reading, metric_count>& r, uint64_t now,
                   uint32_t stale, const Thresholds& l) {
    const auto& c = r[2]; const auto& v = r[3];
    // A partial reading must not result in an unqualified healthy badge.
    if ((c.fresh(now, stale) && c.value >= l.co2_critical) ||
        (v.fresh(now, stale) && v.value >= l.voc_critical)) return Health::Critical;
    if ((c.fresh(now, stale) && c.value >= l.co2_warn) ||
        (v.fresh(now, stale) && v.value >= l.voc_warn)) return Health::Warning;
    if (!c.fresh(now, stale) || !v.fresh(now, stale)) return Health::Unknown;
    return Health::Healthy;
}
bool SensorParser::feed(uint8_t b, uint64_t now, Sample& out) {
    if (b) {
        if (used_ < frame_.size() && !discard_) frame_[used_++] = b;
        else discard_ = true;
        return false;
    }
    if (discard_) { discard_ = false; used_ = 0; ++rejected_; return false; }
    if (!used_) return false;
    uint8_t decoded[8]{};
    size_t pos = 0, n = 0;
    bool ok = true;
    while (pos < used_ && ok) {
        uint8_t code = frame_[pos++];
        if (!code || pos + code - 1 > used_) { ok = false; break; }
        for (unsigned i = 1; i < code; ++i) {
            if (n >= sizeof(decoded)) { ok = false; break; }
            decoded[n++] = frame_[pos++];
        }
        if (ok && code != 255 && pos < used_) {
            if (n >= sizeof(decoded)) ok = false;
            else decoded[n++] = 0;
        }
    }
    used_ = 0;
    if (!ok || n != 5) { ++rejected_; return false; }
    Metric metric;
    switch (decoded[0]) {
        case 0xB3: metric = Metric::Temperature; break;
        case 0xB4: metric = Metric::Humidity; break;
        case 0xB2: metric = Metric::Co2; break;
        case 0xB5: metric = Metric::Voc; break;
        default: ++rejected_; return false;
    }
    const uint32_t bits = uint32_t(decoded[1]) | uint32_t(decoded[2]) << 8 |
                          uint32_t(decoded[3]) << 16 | uint32_t(decoded[4]) << 24;
    float value;
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    std::memcpy(&value, &bits, sizeof(value));
    if (!valid(metric, value)) { ++rejected_; return false; }
    out = {metric, value, now};
    return true;
}
void History::add(const Sample& s) {
    if (!valid(s.metric, s.value)) return;
    const uint64_t minute = s.at_ms / 60000;
    auto& b = buckets_[minute % capacity];
    if (b.minute_plus_one != minute + 1) { b = {}; b.minute_plus_one = minute + 1; }
    const auto m = size_t(s.metric);
    b.sum[m] += s.value; ++b.count[m];
}
void History::chart(Metric metric, uint64_t now, uint32_t window, float* out, size_t n) const {
    if (!n) return;
    std::fill(out, out + n, NAN);
    if (size_t(metric) >= metric_count || !window || window > capacity) return;
    const uint64_t end = now / 60000;
    // Age-based projection remains correct before a full window has elapsed since boot.
    constexpr size_t max_chart_points = 96;
    std::array<float, max_chart_points> sum{};
    std::array<uint32_t, max_chart_points> count{};
    if (n > max_chart_points) return;
    for (const auto& b : buckets_) {
        if (!b.minute_plus_one || b.minute_plus_one - 1 > end) continue;
        const auto age = end - (b.minute_plus_one - 1);
        if (age >= window || !b.count[size_t(metric)]) continue;
        const size_t i = n - 1 - std::min(n - 1, size_t(age * n / window));
        sum[i] += b.sum[size_t(metric)]; count[i] += b.count[size_t(metric)];
    }
    for (size_t i = 0; i < n; ++i) if (count[i]) out[i] = sum[i] / count[i];
}
template<size_t N> static bool terminated(const char (&s)[N]) { return std::memchr(s, 0, N); }
bool validate_settings(const Settings& s) {
    const auto& l = s.limits;
    if (s.version != 1 || !terminated(s.ssid) || !terminated(s.wifi_password) ||
        !terminated(s.mqtt_uri) || !terminated(s.mqtt_user) || !terminated(s.mqtt_password) ||
        !terminated(s.device_id) || !terminated(s.reticulum_url) ||
        !terminated(s.reticulum_token) || !terminated(s.lora_topic) || !terminated(s.ntp_server)) return false;
    if (s.refresh_ms < 250 || s.refresh_ms > 60000 || s.stale_ms < 15000 ||
        s.stale_ms > 300000 || s.publish_seconds < 5 || s.publish_seconds > 3600 ||
        s.brightness < 5 || s.brightness > 100 || s.screen_timeout_s > 3600 ||
        (s.rotation_s && s.rotation_s < 10)) return false;
    if (!std::isfinite(l.co2_warn) || !std::isfinite(l.co2_critical) ||
        !std::isfinite(l.voc_warn) || !std::isfinite(l.voc_critical) ||
        !std::isfinite(l.temp_low) || !std::isfinite(l.temp_high) ||
        !std::isfinite(l.rh_low) || !std::isfinite(l.rh_high)) return false;
    if (l.co2_warn < 400 || l.co2_critical <= l.co2_warn || l.co2_critical > 40000 ||
        l.voc_warn < 1 || l.voc_critical <= l.voc_warn || l.voc_critical > 500 ||
        l.temp_low < -40 || l.temp_high > 85 || l.temp_low >= l.temp_high ||
        l.rh_low < 0 || l.rh_high > 100 || l.rh_low >= l.rh_high) return false;
    if (!s.device_id[0]) return false;
    for (const char* p = s.device_id; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    if (s.mqtt_uri[0] && std::strncmp(s.mqtt_uri, "mqtt://", 7) &&
        std::strncmp(s.mqtt_uri, "mqtts://", 8)) return false;
    for (const auto& h : s.hosts) {
        if (!terminated(h.name) || !terminated(h.address) || !h.port ||
            (h.check != Check::Tcp && h.check != Check::Http)) return false;
        if (h.address[0] && h.check == Check::Http &&
            std::strncmp(h.address, "http://", 7) && std::strncmp(h.address, "https://", 8)) return false;
    }
    return sniffer::valid(s.sniffer);
}
} // namespace dash
