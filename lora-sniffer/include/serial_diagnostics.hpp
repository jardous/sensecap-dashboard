#pragma once
#include <cstddef>
#include <cstdint>

namespace receiver {
enum class SerialCommand { Invalid, Help, Status, Wifi, WatchOn, WatchOff, Provision };
SerialCommand serial_command(const char* text, size_t size);
const char* wifi_reason_name(uint16_t reason);

// Deliberately contains no password/token or pointer to the provisioning blob.
struct DisconnectRecord { uint64_t at_ms=0; uint16_t reason=0; int rssi_dbm=0; };
struct NetworkDiagnostics {
    bool configured=false, started=false, associated=false, ip_ready=false, ap_info_valid=false;
    bool connecting=false, server_ready=false;
    char ssid[33]{}, bssid[18]{}, mac[18]{}, ip[16]{}, gateway[16]{}, netmask[16]{}, dns[16]{};
    char boot_id[17]{};
    uint32_t reset_reason=0, free_heap=0, console_stack_min_free=0, event_stack_min_free=0;
    int rssi_dbm=0, channel=0, auth_mode=0, power_save=-1, last_connect_error=0;
    uint32_t attempts=0, associations=0, ip_acquisitions=0, disconnects=0, attempt_timeouts=0;
    uint32_t ws_authenticated=0, ws_auth_rejected=0, ws_clients=0;
    uint64_t attempt_started_ms=0, retry_at_ms=0, connected_at_ms=0;
    DisconnectRecord recent[8]{}; // newest first; zero timestamp is unused
};
// Returns zero rather than truncated/invalid JSON if the output buffer is small.
size_t encode_network_diagnostics(const NetworkDiagnostics& state, uint64_t now,
                                 char* output, size_t capacity);
NetworkDiagnostics network_diagnostics();
void serial_console(); // owns USB input, never calls the radio or UI directly
}
