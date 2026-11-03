#pragma once
#include "core/model.hpp"
namespace dash::lora {
void start(const sniffer::ClientSettings& settings);
// UI owner drains at most 8 events per iteration, never performs networking.
void poll(sniffer::State& state);
bool apply_radio(const sniffer::RadioConfig& config);
}
