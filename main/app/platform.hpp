#pragma once
#include "core/model.hpp"
namespace dash::platform {
uint64_t now_ms();
void start(const Settings& settings);
bool next_sample(Sample& sample);
void network_snapshot(NetworkState& state);
// Asynchronous bounded queue. Never writes flash or connects in a UI callback.
bool save_settings(const Settings& settings);
// Local USB setup changes only the sniffer NVS blob, not the legacy settings.
bool provision_sniffer_link(const sniffer::ClientSettings& settings);
void publish(const ViewModel& model);
Settings load_settings();
void apply_backlight(uint8_t percent);
void start_screen_button();
bool take_screen_toggle();
}
