#include "client.hpp"
#include "network/internal.hpp"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace dash::lora {
namespace {
sniffer::ClientSettings settings;
QueueHandle_t events, commands;
std::atomic<bool> up{false};
std::atomic<uint32_t> dropped{0}, malformed{0};
std::atomic<int> diagnostic{0};
sniffer::MessageAssembler assembler;
esp_websocket_client_handle_t client;
bool telemetry_reported=false; // WebSocket callback task only
void callback(void*,esp_event_base_t,int32_t id,void* data) {
    if (id==WEBSOCKET_EVENT_CONNECTED) {
        up=true; diagnostic=1; assembler.reset(); telemetry_reported=false;
        ESP_LOGI("sniffer_link","Socket connected; awaiting authenticated telemetry");
    }
    else if (id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_CLOSED) { up=false; if (diagnostic!=3) diagnostic=2; assembler.reset(); }
    else if (id==WEBSOCKET_EVENT_ERROR) {
        const auto* e=static_cast<esp_websocket_event_data_t*>(data);
        if (e && e->error_handle.esp_ws_handshake_status_code==401) diagnostic=3;
        else if (diagnostic!=3) diagnostic=4;
    } else if (id==WEBSOCKET_EVENT_DATA) {
        const auto* e=static_cast<esp_websocket_event_data_t*>(data);
        if (!e || e->data_len<0 || e->payload_len<0 || e->payload_offset<0) { ++malformed; return; }
        if (assembler.feed(e->op_code,e->fin,e->payload_len,e->payload_offset,e->data_ptr,e->data_len)) {
            sniffer::Event event;
            if (!sniffer::decode(assembler.data(),assembler.size(),event)) ++malformed;
            else {
                if (event.type==sniffer::EventType::Error && !std::strcmp(event.message,"Authentication failed")) { diagnostic=3; up=false; }
                if (!telemetry_reported && event.type==sniffer::EventType::Status) {
                    telemetry_reported=true;
                    ESP_LOGI("sniffer_link","Authenticated telemetry received; radio_ready=%s",event.radio_ready?"true":"false");
                }
                if (xQueueSend(events,&event,0)!=pdTRUE) ++dropped;
            }
        } else if (assembler.rejected()) ++malformed;
    }
}
void worker(void*) {
    char host[128],header[128];
    std::snprintf(host,sizeof(host),"ws://%s:%u/lora",settings.host,settings.port);
    std::snprintf(header,sizeof(header),"Authorization: Bearer %s\r\n",settings.token);
    esp_websocket_client_config_t cfg{};
    cfg.uri=host; cfg.headers=header; cfg.buffer_size=512; cfg.task_stack=8192; cfg.task_prio=3;
    cfg.network_timeout_ms=settings.timeout_ms; cfg.reconnect_timeout_ms=3000;
    cfg.ping_interval_sec=5; cfg.pingpong_timeout_sec=10;
    client=esp_websocket_client_init(&cfg);
    if (!client) { diagnostic=4; vTaskDelete(nullptr); return; }
    ESP_ERROR_CHECK(esp_websocket_register_events(client,WEBSOCKET_EVENT_ANY,callback,nullptr));
    bool started=false, sent=false;
    uint64_t last_config=0;
    char message[sniffer::max_message+1];
    sniffer::RadioConfig command;
    bool pending=false;
    for (;;) {
        if (platform::wifi_connected() && !started) {
            if (esp_websocket_client_start(client)==ESP_OK) started=true;
        }
        if (!up) sent=false;
        if (xQueueReceive(commands,&command,0)==pdTRUE) pending=true;
        if (settings.apply_on_connect && up && !sent && !pending) { command=settings.radio; pending=true; }
        if (pending && up && platform::now_ms()-last_config>=1000) {
            const auto size=sniffer::encode_config(command,message,sizeof(message));
            if (size && esp_websocket_client_send_text(client,message,size,pdMS_TO_TICKS(1000))==int(size)) sent=true;
            else diagnostic=4;
            last_config=platform::now_ms();
            pending=false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}
void start(const sniffer::ClientSettings& s) {
    settings=s;
    events=xQueueCreate(16,sizeof(sniffer::Event)); commands=xQueueCreate(1,sizeof(sniffer::RadioConfig));
    configASSERT(events && commands);
    if (settings.enabled && sniffer::valid(settings)) configASSERT(xTaskCreate(worker,"sniffer_ws",6144,nullptr,3,nullptr)==pdPASS);
}
bool apply_radio(const sniffer::RadioConfig& c) { return settings.enabled && up && sniffer::valid(c) && xQueueSend(commands,&c,0)==pdTRUE; }
void poll(sniffer::State& s) {
    s.enabled=settings.enabled; s.connected=up;
    s.local_drops=dropped; s.malformed=malformed;
    sniffer::Event event;
    for (unsigned i=0;i<8 && xQueueReceive(events,&event,0)==pdTRUE;++i) s.apply(event,platform::now_ms(),settings.history_size);
    const char* message=!settings.enabled?"Disabled - open settings":!platform::wifi_connected()?"D1S Wi-Fi disconnected":
        diagnostic==3?"Authentication failed":diagnostic==4?"Connection or send error":!up?"Connecting - check host and token":
        (!s.last_message_ms || platform::now_ms()-s.last_message_ms>settings.timeout_ms)?"Connected - telemetry stale":
        !s.radio_ready?"Connected - radio unavailable":"Receiving - passive only";
    std::snprintf(s.diagnostic,sizeof(s.diagnostic),"%s",message);
}
}
