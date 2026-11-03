#include "sniffer.hpp"
#include "serial_diagnostics.hpp"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace receiver {
namespace {
Provision credentials;
httpd_handle_t server;
std::atomic<bool> wifi_up{false};
std::atomic<bool> wifi_connecting{false};
portMUX_TYPE diagnostic_lock=portMUX_INITIALIZER_UNLOCKED;
NetworkDiagnostics diagnostic;
esp_netif_t* station=nullptr;
std::atomic<uint32_t> ws_authenticated{0},ws_auth_rejected{0},ws_clients{0};
static const uint32_t authenticated_tag=0x52584F4E;
std::array<int,2> clients{{-1,-1}}; // accessed only on HTTP server task
struct Job { std::atomic<bool> used{false}; size_t size=0; std::array<char,sniffer::max_message+1> data{}; };
std::array<Job,4> jobs;
sniffer::MessageAssembler assemblers[2]; // bounded, one per authorized session
uint64_t last_command[2]{};
bool authorized(httpd_req_t* request) {
    char actual[128]{};
    const size_t n=httpd_req_get_hdr_value_len(request,"Authorization");
    if (!n || n>=sizeof(actual) || httpd_req_get_hdr_value_str(request,"Authorization",actual,sizeof(actual))!=ESP_OK) return false;
    return sniffer::bearer_matches(actual,n,credentials.token,sizeof(credentials.token));
}
void send_error(httpd_req_t* req,const char* reason) {
    sniffer::Event error; error.type=sniffer::EventType::Error;
    std::snprintf(error.boot_id,sizeof(error.boot_id),"%s",boot_id);
    std::snprintf(error.message,sizeof(error.message),"%s",reason);
    char text[256]; const size_t n=sniffer::encode(error,text,sizeof(text));
    httpd_ws_frame_t frame{}; frame.type=HTTPD_WS_TYPE_TEXT; frame.payload=reinterpret_cast<uint8_t*>(text); frame.len=n;
    httpd_ws_send_frame(req,&frame);
}
esp_err_t websocket(httpd_req_t* req) {
    const int fd=httpd_req_to_sockfd(req);
    if (req->method==HTTP_GET) {
        // IDF5.1 performs Upgrade before this handler. No telemetry/session is
        // registered until Authorization passes. All sends run on this same task.
        if (httpd_ws_get_fd_info(server,fd)!=HTTPD_WS_CLIENT_WEBSOCKET) return ESP_FAIL;
        if (!authorized(req)) { ++ws_auth_rejected; send_error(req,"Authentication failed"); return ESP_FAIL; }
        // A newly opened socket may reuse an old client's descriptor.
        for (auto& old:clients) if (old==fd) old=-1;
        size_t i=0;
        for (;i<clients.size();++i) if (clients[i]<0 || httpd_ws_get_fd_info(server,clients[i])!=HTTPD_WS_CLIENT_WEBSOCKET) break;
        if (i==clients.size()) { send_error(req,"Client limit reached"); return ESP_FAIL; }
        clients[i]=fd; assemblers[i].reset(); last_command[i]=0;
        ++ws_authenticated; ++ws_clients;
        req->sess_ctx=const_cast<uint32_t*>(&authenticated_tag); req->free_ctx=[](void*){ --ws_clients; };
        return ESP_OK;
    }
    size_t slot=0; for (;slot<clients.size();++slot) if (clients[slot]==fd) break;
    if (slot==clients.size() || req->sess_ctx!=&authenticated_tag) return ESP_FAIL;
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(req,&frame,0)!=ESP_OK || frame.len>sniffer::max_message) return ESP_FAIL;
    uint8_t bytes[sniffer::max_message+1]; frame.payload=bytes;
    // max_len=0 means "read a header" in IDF 5.1, not "read no payload".
    // Empty final continuations/pings/closes must not read the next header twice.
    if (frame.len && httpd_ws_recv_frame(req,&frame,frame.len)!=ESP_OK) return ESP_FAIL;
    if (frame.type==HTTPD_WS_TYPE_CLOSE) { clients[slot]=-1; return ESP_FAIL; }
    if (frame.type==HTTPD_WS_TYPE_PING) { frame.type=HTTPD_WS_TYPE_PONG; return httpd_ws_send_frame(req,&frame); }
    if (frame.type==HTTPD_WS_TYPE_PONG) return ESP_OK;
    if (frame.type!=HTTPD_WS_TYPE_TEXT && frame.type!=HTTPD_WS_TYPE_CONTINUE) return ESP_FAIL;
    if (assemblers[slot].feed(frame.type,frame.final,frame.len,0,reinterpret_cast<char*>(bytes),frame.len)) {
        sniffer::RadioConfig config;
        if ((last_command[slot] && now_ms()-last_command[slot]<1000) || !sniffer::decode_config(assemblers[slot].data(),assemblers[slot].size(),config)) {
            send_error(req,"Invalid or rate limited configuration"); return ESP_OK;
        }
        last_command[slot]=now_ms();
        if (xQueueSend(commands,&config,0)!=pdTRUE) send_error(req,"Radio configuration busy");
    } else if (assemblers[slot].rejected()) return ESP_FAIL;
    return ESP_OK;
}
void broadcast(void* arg) {
    auto& job=*static_cast<Job*>(arg);
    httpd_ws_frame_t frame{}; frame.type=HTTPD_WS_TYPE_TEXT;
    frame.payload=reinterpret_cast<uint8_t*>(job.data.data()); frame.len=job.size;
    for (auto& fd:clients) {
        if (fd<0) continue;
        if (httpd_ws_get_fd_info(server,fd)!=HTTPD_WS_CLIENT_WEBSOCKET || httpd_sess_get_ctx(server,fd)!=&authenticated_tag) { fd=-1; continue; }
        if (httpd_ws_send_frame_async(server,fd,&frame)!=ESP_OK) {
            count_send_drop(); httpd_sess_trigger_close(server,fd); fd=-1;
        }
    }
    job.used=false;
}
void publish(const sniffer::Event& e) {
    for (auto& job:jobs) {
        bool expected=false;
        if (!job.used.compare_exchange_strong(expected,true)) continue;
        job.size=sniffer::encode(e,job.data.data(),job.data.size());
        if (!job.size || httpd_queue_work(server,broadcast,&job)!=ESP_OK) { job.used=false; count_send_drop(); }
        return;
    }
    count_send_drop(); // exhausted bounded send-job pool, not a known over-air loss
}
void event(void*,esp_event_base_t base,int32_t id,void* data) {
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_START) {
        portENTER_CRITICAL(&diagnostic_lock); diagnostic.started=true; portEXIT_CRITICAL(&diagnostic_lock);
    }
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_CONNECTED) {
        portENTER_CRITICAL(&diagnostic_lock);
        diagnostic.associated=true; ++diagnostic.associations;
        portEXIT_CRITICAL(&diagnostic_lock);
    }
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_up=false; wifi_connecting=false;
        const auto* reason=static_cast<wifi_event_sta_disconnected_t*>(data);
        portENTER_CRITICAL(&diagnostic_lock);
        diagnostic.associated=false; diagnostic.ip_ready=false;
        ++diagnostic.disconnects;
        for (size_t i=7;i>0;--i) diagnostic.recent[i]=diagnostic.recent[i-1];
        diagnostic.recent[0]={now_ms(),uint16_t(reason?reason->reason:0),reason?reason->rssi:0};
        portEXIT_CRITICAL(&diagnostic_lock);
        ESP_LOGW("wifi","Disconnected; reconnect scheduled, reason=%u (%s)",reason?unsigned(reason->reason):0u,
            wifi_reason_name(reason?reason->reason:0));
    }
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        wifi_up=true; wifi_connecting=false;
        const auto* ip=static_cast<ip_event_got_ip_t*>(data);
        char address[16]{},gateway[16]{},netmask[16]{};
        if (ip) {
            std::snprintf(address,sizeof(address),IPSTR,IP2STR(&ip->ip_info.ip));
            std::snprintf(gateway,sizeof(gateway),IPSTR,IP2STR(&ip->ip_info.gw));
            std::snprintf(netmask,sizeof(netmask),IPSTR,IP2STR(&ip->ip_info.netmask));
        }
        portENTER_CRITICAL(&diagnostic_lock);
        diagnostic.ip_ready=true; diagnostic.connected_at_ms=now_ms(); ++diagnostic.ip_acquisitions;
        std::memcpy(diagnostic.ip,address,sizeof(address)); std::memcpy(diagnostic.gateway,gateway,sizeof(gateway));
        std::memcpy(diagnostic.netmask,netmask,sizeof(netmask));
        portEXIT_CRITICAL(&diagnostic_lock);
        ESP_LOGI("wifi","Local Wi-Fi connected; WebSocket endpoint port8080 /lora");
    }
    if (base==IP_EVENT && id==IP_EVENT_STA_LOST_IP) {
        // Do not call connect() on an already associated station. The normal
        // disconnect event/worker will schedule a fresh, bounded attempt.
        wifi_up=false; esp_wifi_disconnect();
        portENTER_CRITICAL(&diagnostic_lock); diagnostic.ip_ready=false; portEXIT_CRITICAL(&diagnostic_lock);
    }
    const auto headroom=uxTaskGetStackHighWaterMark(nullptr);
    portENTER_CRITICAL(&diagnostic_lock); diagnostic.event_stack_min_free=headroom; portEXIT_CRITICAL(&diagnostic_lock);
}
void worker(void*) {
    uint64_t last_status=0,retry=0,connect_started=0; uint32_t backoff=1000;
    for (;;) {
        const auto now=now_ms();
        if (!wifi_up && !wifi_connecting && now>=retry) {
            wifi_connecting=true; connect_started=now;
            const auto error=esp_wifi_connect();
            if (error!=ESP_OK) wifi_connecting=false;
            retry=now+backoff; backoff=std::min(uint32_t(32000),uint32_t(backoff*2));
            portENTER_CRITICAL(&diagnostic_lock);
            ++diagnostic.attempts; diagnostic.attempt_started_ms=now;
            diagnostic.last_connect_error=error; diagnostic.retry_at_ms=retry;
            portEXIT_CRITICAL(&diagnostic_lock);
        }
        if (!wifi_up && wifi_connecting && now-connect_started>=30000) {
            esp_wifi_disconnect(); wifi_connecting=false; retry=now+backoff;
            portENTER_CRITICAL(&diagnostic_lock);
            ++diagnostic.attempt_timeouts; diagnostic.retry_at_ms=retry;
            portEXIT_CRITICAL(&diagnostic_lock);
        }
        if (wifi_up) backoff=1000;
        sniffer::Event e;
        for (unsigned i=0;i<8 && xQueueReceive(telemetry,&e,0)==pdTRUE;++i) {
            if (wifi_up) publish(e); else count_send_drop();
        }
        if (wifi_up && now-last_status>=1000) { publish(radio_status()); last_status=now; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}
NetworkDiagnostics network_diagnostics() {
    portENTER_CRITICAL(&diagnostic_lock); auto result=diagnostic; portEXIT_CRITICAL(&diagnostic_lock);
    result.connecting=wifi_connecting.load();
    result.ws_authenticated=ws_authenticated.load(); result.ws_auth_rejected=ws_auth_rejected.load();
    result.ws_clients=ws_clients.load();
    if (result.configured) {
        wifi_ap_record_t ap{};
        result.ap_info_valid=result.associated && esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
        if (result.ap_info_valid) {
            result.rssi_dbm=ap.rssi; result.channel=ap.primary; result.auth_mode=ap.authmode;
            std::snprintf(result.bssid,sizeof(result.bssid),"%02x:%02x:%02x:%02x:%02x:%02x",
                ap.bssid[0],ap.bssid[1],ap.bssid[2],ap.bssid[3],ap.bssid[4],ap.bssid[5]);
        }
        wifi_ps_type_t power_save;
        if (esp_wifi_get_ps(&power_save)==ESP_OK) result.power_save=int(power_save);
        esp_netif_dns_info_t dns{};
        if (station && result.ip_ready && esp_netif_get_dns_info(station,ESP_NETIF_DNS_MAIN,&dns)==ESP_OK &&
            dns.ip.type==ESP_IPADDR_TYPE_V4) std::snprintf(result.dns,sizeof(result.dns),IPSTR,IP2STR(&dns.ip.u_addr.ip4));
    }
    return result;
}
void network_start(const Provision& p) {
    credentials=p;
    portENTER_CRITICAL(&diagnostic_lock);
    diagnostic.configured=true; std::memcpy(diagnostic.ssid,p.ssid,sizeof(p.ssid));
    portEXIT_CRITICAL(&diagnostic_lock);
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    station=esp_netif_create_default_wifi_sta(); configASSERT(station);
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,event,nullptr));
    wifi_config_t cfg{}; std::memcpy(cfg.sta.ssid,p.ssid,sizeof(cfg.sta.ssid)); std::memcpy(cfg.sta.password,p.password,sizeof(cfg.sta.password));
    cfg.sta.pmf_cfg.capable=true; // match the working D1S; negotiate PMF with WPA3 APs
    cfg.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.failure_retry_cnt=3; // permit AP comeback/retry before trying another matching AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&cfg)); ESP_ERROR_CHECK(esp_wifi_start());
    uint8_t mac[6]{}; ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA,mac));
    char mac_text[18]; std::snprintf(mac_text,sizeof(mac_text),"%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    portENTER_CRITICAL(&diagnostic_lock); std::memcpy(diagnostic.mac,mac_text,sizeof(mac_text)); portEXIT_CRITICAL(&diagnostic_lock);
    ESP_ERROR_CHECK(mdns_init()); ESP_ERROR_CHECK(mdns_hostname_set("lora-sniffer"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Passive LoRa Sniffer"));
    ESP_ERROR_CHECK(mdns_service_add(nullptr,"_lora-sniffer","_tcp",8080,nullptr,0));
    httpd_config_t config=HTTPD_DEFAULT_CONFIG(); config.server_port=8080; config.max_open_sockets=4;
    config.stack_size=8192; config.recv_wait_timeout=1; config.send_wait_timeout=1; config.lru_purge_enable=true;
    ESP_ERROR_CHECK(httpd_start(&server,&config));
    httpd_uri_t route{}; route.uri="/lora"; route.method=HTTP_GET; route.handler=websocket;
    route.is_websocket=true; route.handle_ws_control_frames=true;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&route));
    portENTER_CRITICAL(&diagnostic_lock); diagnostic.server_ready=true; portEXIT_CRITICAL(&diagnostic_lock);
    configASSERT(xTaskCreate(worker,"sniffer_network",6144,nullptr,3,nullptr)==pdPASS);
}
}
