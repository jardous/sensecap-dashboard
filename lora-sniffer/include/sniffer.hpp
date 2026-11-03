#pragma once
#include "lora/protocol.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
namespace receiver {
using Provision=sniffer::Provision;
extern QueueHandle_t telemetry, commands;
extern char boot_id[17];
uint64_t now_ms();
void radio_start();
sniffer::Event radio_status();
void count_send_drop();
void network_start(const Provision& provision);
bool provision_valid(const Provision& provision);
}
