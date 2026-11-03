#include "serial_diagnostics.hpp"
#include "lora/protocol.hpp"
#include "cJSON.h"
#include <cassert>
#include <cstring>
#include <string>

using namespace receiver;
int main() {
    for (const auto& command:{"help","?","status","wifi","watch on","watch off"," status\r"})
        assert(serial_command(command,std::strlen(command))!=SerialCommand::Invalid);
    for (const auto& command:{"","reboot","scan","connect","set_config","status junk","STATUS","status\nprovision"})
        assert(serial_command(command,std::strlen(command))==SerialCommand::Invalid);
    assert(serial_command(nullptr,10)==SerialCommand::Invalid);
    assert(serial_command("status\0extra",12)==SerialCommand::Invalid);
    assert(serial_command(std::string(512,'x').data(),512)==SerialCommand::Invalid);
    assert(serial_command("{bad",4)==SerialCommand::Provision);
    sniffer::Provision provision;
    assert(!sniffer::decode_provision("{bad",4,provision));
    assert(!std::strcmp(wifi_reason_name(204),"HANDSHAKE_TIMEOUT"));
    assert(!std::strcmp(wifi_reason_name(999),"UNKNOWN"));

    char output[4096];
    NetworkDiagnostics s;
    assert(encode_network_diagnostics(s,1000,output,sizeof(output)));
    auto* json=cJSON_Parse(output); assert(json);
    assert(!std::strcmp(cJSON_GetObjectItem(json,"state")->valuestring,"unprovisioned"));
    assert(cJSON_IsNull(cJSON_GetObjectItem(json,"rssi_dbm")));
    assert(!cJSON_GetObjectItem(json,"password") && !cJSON_GetObjectItem(json,"token"));
    cJSON_Delete(json);
    s.configured=true; s.started=true; s.connecting=true; s.attempt_started_ms=500; s.retry_at_ms=2000;
    std::strcpy(s.ssid,"a\"b\n\\c"); std::strcpy(s.ip,"192.0.2.57");
    s.rssi_dbm=-42; // a stale RSSI/IP must never look live after disconnect
    assert(encode_network_diagnostics(s,1000,output,sizeof(output)));
    json=cJSON_Parse(output); assert(json);
    assert(!std::strcmp(cJSON_GetObjectItem(json,"state")->valuestring,"connecting"));
    assert(!std::strcmp(cJSON_GetObjectItem(json,"ssid")->valuestring,s.ssid));
    assert(!std::strcmp(cJSON_GetObjectItem(json,"ip")->valuestring,""));
    assert(cJSON_IsNull(cJSON_GetObjectItem(json,"rssi_dbm")));
    assert(cJSON_GetObjectItem(json,"attempt_age_ms")->valueint==500);
    assert(cJSON_GetObjectItem(json,"retry_in_ms")->valueint==0);
    cJSON_Delete(json);
    for (unsigned i=0;i<8;++i) s.recent[i]={uint64_t(i+1),204,-80};
    s.associated=true; s.ip_ready=true; s.ap_info_valid=true; s.connecting=false;
    s.connected_at_ms=100; s.rssi_dbm=-78;
    assert(encode_network_diagnostics(s,1000,output,sizeof(output)));
    json=cJSON_Parse(output); assert(json);
    assert(!std::strcmp(cJSON_GetObjectItem(json,"state")->valuestring,"connected"));
    assert(cJSON_GetObjectItem(json,"rssi_dbm")->valueint==-78);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(json,"recent_disconnects"))==8);
    cJSON_Delete(json);
    char small[32]; assert(!encode_network_diagnostics(s,1000,small,sizeof(small)) && !small[0]);
    assert(!encode_network_diagnostics(s,1000,nullptr,4096));
    // The console shares the already tested bounded, NUL-rejecting setup framer.
    sniffer::SetupLine line;
    for (unsigned i=0;i<600;++i) line.feed('x');
    assert(line.feed('\n')==sniffer::SetupLine::Result::Discarded);
    for (auto c:std::string("wifi\r")) line.feed(c);
    assert(line.feed('\n')==sniffer::SetupLine::Result::Complete);
    assert(serial_command(line.data(),line.size())==SerialCommand::Wifi);
}
