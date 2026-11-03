#pragma once
// Seeed B2B XIAO ESP32S3 + Wio-SX1262, NOT nRF52840/header boards.
// Verified against Seeed one_channel_hub and matching Meshtastic variant.
namespace board {
constexpr int sck=7, miso=8, mosi=9, nss=41, reset=42, busy=40, dio1=39, rx_enable=38;
constexpr float tcxo_voltage=1.8f; // module datasheet allows 1.7..3.3V; Meshtastic uses1.8
}
