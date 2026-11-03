#include "core/model.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
using namespace dash;
static std::vector<uint8_t> packet(uint8_t type, float value) {
    uint32_t bits; std::memcpy(&bits, &value, 4);
    const uint8_t raw[] = {type, uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24)};
    std::vector<uint8_t> out(1); size_t code_at = 0; uint8_t code = 1;
    for (uint8_t b : raw) {
        if (b == 0) { out[code_at] = code; code_at = out.size(); out.push_back(0); code = 1; }
        else { out.push_back(b); ++code; }
    }
    out[code_at] = code; out.push_back(0); return out;
}
int main() {
    CHECK(std::abs(dew_point(25, 50) - 13.85f) < .1f);
    CHECK(std::abs(absolute_humidity(25, 50) - 11.51f) < .1f);
    CHECK(std::abs(dew_point(10, 100) - 10) < .001f);
    CHECK(absolute_humidity(20, 0) == 0);
    CHECK(std::isnan(dew_point(20, 0)));
    CHECK(std::isnan(absolute_humidity(NAN, 50)));
    CHECK(std::isnan(dew_point(20, 101)));
    SensorParser parser; Sample s;
    for (const auto type : {0xB3, 0xB4, 0xB2, 0xB5}) {
        const float value = type == 0xB2 ? 612 : 23.5f;
        int emitted = 0;
        for (uint8_t b : packet(type, value)) emitted += parser.feed(b, 1234, s);
        CHECK(emitted == 1); CHECK(s.value == value && s.at_ms == 1234);
    }
    for (const auto value : {NAN, INFINITY, -1000.f, 0.f, 40001.f})
        for (uint8_t b : packet(0xB2, value)) CHECK(!parser.feed(b, 0, s));
    for (uint8_t b : packet(0xB5, 0)) CHECK(!parser.feed(b, 0, s));
    for (uint8_t b : packet(0xB0, 23)) CHECK(!parser.feed(b, 0, s)); // do not substitute SCD temp
    for (int i = 0; i < 100; ++i) CHECK(!parser.feed(0xFF, 0, s));
    CHECK(!parser.feed(0, 0, s));
    CHECK(!parser.feed(8, 0, s)); CHECK(!parser.feed(1, 0, s)); CHECK(!parser.feed(0, 0, s));
    int emitted = 0;
    for (uint8_t b : packet(0xB3, 0)) emitted += parser.feed(b, 5000, s);
    CHECK(emitted == 1 && s.value == 0);
    CHECK(parser.rejected() >= 9);
    // Random corruption / re-synchronization, including fragmented COBS blocks.
    std::mt19937 random(42);
    for (int i = 0; i < 100000; ++i) parser.feed(uint8_t(random()), 5000, s);
    parser.feed(0, 5000, s); emitted = 0;
    for (uint8_t b : packet(0xB4, 45.2f)) emitted += parser.feed(b, 5001, s);
    CHECK(emitted == 1 && s.metric == Metric::Humidity);
    Reading r{23, 1000}; CHECK(r.fresh(1001, 500)); CHECK(!r.fresh(2000, 500)); CHECK(!r.fresh(0, 500));
    auto history = new History;
    history->add({Metric::Temperature, 20, 0}); history->add({Metric::Temperature, 24, 500});
    float values[60]; history->chart(Metric::Temperature, 1000, 60, values, 60);
    CHECK(values[59] == 22 && std::isnan(values[58]));
    history->chart(Metric::Humidity, 1000, 60, values, 60); CHECK(std::isnan(values[59]));
    history->add({Metric::Temperature, 30, 1440ull * 60000});
    history->chart(Metric::Temperature, 1440ull * 60000, 1440, values, 60);
    CHECK(values[59] == 30 && std::isnan(values[0])); delete history;
    auto h2 = new History;
    for (uint64_t minute = 0; minute < 1440; ++minute) h2->add({Metric::Co2, float(500 + minute), minute * 60000});
    for (uint32_t window : {60, 360, 1440}) {
        h2->chart(Metric::Co2, 1439ull * 60000, window, values, 60);
        CHECK(std::isfinite(values[0]) && values[59] > values[0]);
    }
    h2->chart(Metric::Co2, 3000ull * 60000, 1440, values, 60); CHECK(std::isnan(values[59])); delete h2;
    std::array<Reading, metric_count> readings{}; Thresholds limits;
    CHECK(air_quality(readings, 1000, 20000, limits) == Health::Unknown);
    readings[2] = {600, 1000}; CHECK(air_quality(readings, 1000, 20000, limits) == Health::Unknown);
    readings[3] = {80, 1000}; CHECK(air_quality(readings, 1000, 20000, limits) == Health::Healthy);
    readings[2].value = 1000; CHECK(air_quality(readings, 1000, 20000, limits) == Health::Warning);
    readings[2].value = 1500; CHECK(air_quality(readings, 1000, 20000, limits) == Health::Critical);
    CHECK(air_quality(readings, 50000, 20000, limits) == Health::Unknown);
    Settings config; CHECK(validate_settings(config));
    config.brightness = 0; CHECK(!validate_settings(config)); config.brightness = 85;
    config.limits.voc_warn = NAN; CHECK(!validate_settings(config)); config.limits = {};
    std::memset(config.ssid, 'x', sizeof(config.ssid)); CHECK(!validate_settings(config)); config.ssid[0] = 0;
    std::strcpy(config.device_id, "bad/topic"); CHECK(!validate_settings(config));
    std::puts("Core: calculations, protocol, fuzz, freshness, history and settings passed");
}
