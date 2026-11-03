#include "serial_diagnostics.hpp"
#include "cJSON.h"
#include <cstring>

namespace receiver {
SerialCommand serial_command(const char* text,size_t n) {
    if (!text || !n || n>511 || std::memchr(text,0,n)) return SerialCommand::Invalid;
    while (n && (text[0]==' ' || text[0]=='\t')) { ++text; --n; }
    while (n && (text[n-1]==' ' || text[n-1]=='\t' || text[n-1]=='\r')) --n;
    const auto match=[&](const char* command) { return n==std::strlen(command) && !std::memcmp(text,command,n); };
    if (match("help") || match("?")) return SerialCommand::Help;
    if (match("status")) return SerialCommand::Status;
    if (match("wifi")) return SerialCommand::Wifi;
    if (match("watch on")) return SerialCommand::WatchOn;
    if (match("watch off")) return SerialCommand::WatchOff;
    // This only selects the existing strict provisioning decoder, not a new API.
    if (n && text[0]=='{') return SerialCommand::Provision;
    return SerialCommand::Invalid;
}
const char* wifi_reason_name(uint16_t reason) {
    // ESP-IDF 5.1 wifi_err_reason_t. Always retain the numeric code for unknowns.
    switch (reason) {
    case 0: return "NONE";
    case 1: return "UNSPECIFIED";
    case 2: return "AUTH_EXPIRE";
    case 3: return "AUTH_LEAVE";
    case 4: return "INACTIVITY";
    case 5: return "AP_TOO_MANY_STATIONS";
    case 6: return "NOT_AUTHENTICATED";
    case 7: return "NOT_ASSOCIATED";
    case 8: return "ASSOC_LEAVE";
    case 9: return "ASSOC_NOT_AUTHENTICATED";
    case 14: return "MIC_FAILURE";
    case 15: return "FOUR_WAY_HANDSHAKE_TIMEOUT";
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 23: return "IEEE_802_1X_AUTH_FAILED";
    case 24: return "CIPHER_SUITE_REJECTED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 206: return "AP_TSF_RESET";
    case 207: return "ROAMING";
    case 208: return "ASSOC_COMEBACK_TIME_TOO_LONG";
    case 209: return "SA_QUERY_TIMEOUT";
    default: return "UNKNOWN";
    }
}
size_t encode_network_diagnostics(const NetworkDiagnostics& s,uint64_t now,char* output,size_t capacity) {
    if (!output || capacity<2 || capacity>0x7fffffff) return 0;
    output[0]=0;
    auto* root=cJSON_CreateObject(); if (!root) return 0;
    bool ok=true;
    const auto number=[&](const char* key,double value) { ok=bool(cJSON_AddNumberToObject(root,key,value)) && ok; };
    const auto boolean=[&](const char* key,bool value) { ok=bool(cJSON_AddBoolToObject(root,key,value)) && ok; };
    const auto string=[&](const char* key,const char* value) { ok=bool(cJSON_AddStringToObject(root,key,value)) && ok; };
    const auto nullable=[&](const char* key,bool valid,double value) {
        ok=bool(valid?cJSON_AddNumberToObject(root,key,value):cJSON_AddNullToObject(root,key)) && ok;
    };
    number("schema",1); string("type","usb_wifi"); number("uptime_ms",double(now));
    string("boot_id",s.boot_id); number("reset_reason",s.reset_reason);
    number("free_heap",s.free_heap); number("console_stack_min_free",s.console_stack_min_free);
    number("event_stack_min_free",s.event_stack_min_free);
    const char* state=!s.configured?"unprovisioned":!s.started?"starting":s.ip_ready?"connected":
        s.associated?"waiting_for_ip":s.connecting?"connecting":"disconnected";
    string("state",state); boolean("configured",s.configured); boolean("associated",s.associated);
    boolean("ip_ready",s.ip_ready); string("ssid",s.ssid); string("mac",s.mac);
    string("bssid",s.ap_info_valid?s.bssid:""); nullable("rssi_dbm",s.ap_info_valid,s.rssi_dbm);
    nullable("channel",s.ap_info_valid,s.channel); nullable("auth_mode",s.ap_info_valid,s.auth_mode);
    number("power_save",s.power_save);
    string("ip",s.ip_ready?s.ip:""); string("gateway",s.ip_ready?s.gateway:"");
    string("netmask",s.ip_ready?s.netmask:""); string("dns",s.ip_ready?s.dns:"");
    boolean("server_ready",s.server_ready); string("hostname","lora-sniffer.local");
    number("port",8080); string("path","/lora");
    number("attempts",s.attempts); number("associations",s.associations); number("ip_acquisitions",s.ip_acquisitions);
    number("disconnects",s.disconnects); number("attempt_timeouts",s.attempt_timeouts);
    number("last_connect_error",s.last_connect_error);
    number("attempt_age_ms",s.connecting && now>=s.attempt_started_ms?double(now-s.attempt_started_ms):0);
    number("connected_ms",s.ip_ready && now>=s.connected_at_ms?double(now-s.connected_at_ms):0);
    number("retry_in_ms",!s.ip_ready && !s.connecting && s.retry_at_ms>now?double(s.retry_at_ms-now):0);
    number("ws_authenticated",s.ws_authenticated); number("ws_auth_rejected",s.ws_auth_rejected);
    number("ws_clients",s.ws_clients);
    auto* history=cJSON_AddArrayToObject(root,"recent_disconnects"); ok=bool(history) && ok;
    if (history) for (const auto& record:s.recent) {
        if (!record.at_ms) continue;
        auto* item=cJSON_CreateObject(); if (!item) { ok=false; break; }
        ok=bool(cJSON_AddItemToArray(history,item)) && ok;
        ok=bool(cJSON_AddNumberToObject(item,"at_ms",double(record.at_ms))) && ok;
        ok=bool(cJSON_AddNumberToObject(item,"reason",record.reason)) && ok;
        ok=bool(cJSON_AddStringToObject(item,"name",wifi_reason_name(record.reason))) && ok;
        ok=bool(cJSON_AddNumberToObject(item,"rssi_dbm",record.rssi_dbm)) && ok;
    }
    const bool printed=ok && cJSON_PrintPreallocated(root,output,int(capacity),false);
    cJSON_Delete(root);
    if (!printed) { output[0]=0; return 0; }
    return std::strlen(output);
}
}
