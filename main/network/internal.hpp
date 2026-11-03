#pragma once
#include "app/platform.hpp"
namespace dash::platform {
void start_sensors();
void start_hosts(const Settings& settings);
void update_host(size_t index, bool online, float latency, uint64_t now);
bool wifi_connected();
}
