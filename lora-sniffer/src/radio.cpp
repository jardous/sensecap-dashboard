#include "sniffer.hpp"
#include "esp_hal.hpp"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
namespace receiver {
QueueHandle_t telemetry,commands;
char boot_id[17];
namespace {
TaskHandle_t task;
portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
sniffer::Event status;
EspHal hal;
Module module(&hal,board::nss,board::dio1,board::reset,board::busy);
SX1262 radio(&module); // private to acquisition translation unit
sniffer::RadioConfig config;
uint64_t sequence=0;
uint8_t index=0;
void irq() { BaseType_t wake=pdFALSE; vTaskNotifyGiveFromISR(task,&wake); if (wake) portYIELD_FROM_ISR(); }
void emit(sniffer::Event& e) {
    std::snprintf(e.boot_id,sizeof(e.boot_id),"%s",boot_id); e.mode=config.mode; e.active_profile=index;
    if (xQueueSend(telemetry,&e,0)!=pdTRUE) { portENTER_CRITICAL(&lock); ++status.queue_drops; portEXIT_CRITICAL(&lock); }
}
int16_t listen() { return config.mode==sniffer::Mode::Cad?radio.startChannelScan():radio.startReceive(); }
int16_t configure(bool initialize=false) {
    const auto& p=config.profiles[index];
    int16_t rc;
    if (initialize) {
        radio.setRfSwitchPins(board::rx_enable,RADIOLIB_NC);
        rc=radio.begin(p.frequency_hz/1e6f,p.bw_hz/1000.f,p.sf,p.cr,p.sync_word,0,p.preamble,board::tcxo_voltage,true);
        if (rc==RADIOLIB_ERR_NONE) rc=radio.setDio2AsRfSwitch(true);
    } else {
        rc=radio.standby();
        if (!rc) rc=radio.setFrequency(p.frequency_hz/1e6f);
        if (!rc) rc=radio.setBandwidth(p.bw_hz/1000.f);
        if (!rc) rc=radio.setSpreadingFactor(p.sf);
        if (!rc) rc=radio.setCodingRate(p.cr);
        if (!rc) rc=radio.setSyncWord(p.sync_word);
        if (!rc) rc=radio.setPreambleLength(p.preamble);
    }
    if (!rc) rc=radio.setCRC(p.crc_enabled?2:0);
    if (!rc) { radio.setDio1Action(irq); ulTaskNotifyTake(pdTRUE,0); rc=listen(); }
    portENTER_CRITICAL(&lock);
    status.radio_ready=rc==RADIOLIB_ERR_NONE; status.mode=config.mode; status.active_profile=index;
    status.packet.profile=p;
    portEXIT_CRITICAL(&lock);
    if (rc) ESP_LOGE("radio","Receive configuration failed: %d",rc);
    else ESP_LOGI("radio","RX only: %lu Hz SF%u BW%lu CR4/%u sync=%02X mode=%s",
        (unsigned long)p.frequency_hz,p.sf,(unsigned long)p.bw_hz,p.cr,p.sync_word,sniffer::mode_name(config.mode));
    return rc;
}
void worker(void*) {
    task=xTaskGetCurrentTaskHandle();
    bool ready=configure(true)==RADIOLIB_ERR_NONE;
    uint64_t switched=now_ms(),last_error=0;
    for (;;) {
        const bool signaled=ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50))!=0;
        if (ready && signaled) {
            if (config.mode==sniffer::Mode::Cad) {
                const auto rc=radio.getChannelScanResult();
                const bool detected=rc==RADIOLIB_LORA_DETECTED;
                portENTER_CRITICAL(&lock); ++status.cad_checks; if (detected) ++status.cad_detections; portEXIT_CRITICAL(&lock);
                // Send detections, not every free CAD sample; total checks in status.
                if (detected) {
                    sniffer::Event e; e.type=sniffer::EventType::Cad; e.cad_detected=true;
                    e.packet.timestamp_ms=now_ms(); e.packet.profile=config.profiles[index]; emit(e);
                }
                vTaskDelay(pdMS_TO_TICKS(20)); // bound CAD CPU/telemetry overhead
                ready=listen()==RADIOLIB_ERR_NONE;
            } else {
                sniffer::Event e; e.type=sniffer::EventType::Packet;
                e.packet.timestamp_ms=now_ms(); e.packet.profile=config.profiles[index];
                const size_t n=radio.getPacketLength();
                const auto rc=n<=sniffer::max_payload?radio.readData(e.packet.payload.data(),n):RADIOLIB_ERR_PACKET_TOO_LONG;
                if (rc==RADIOLIB_ERR_NONE) {
                    e.packet.length=n; e.packet.rssi_dbm=radio.getRSSI(); e.packet.snr_db=radio.getSNR();
                    e.packet.sequence=++sequence; e.packet.crc=e.packet.profile.crc_enabled?sniffer::Crc::Ok:sniffer::Crc::Disabled;
                    // Restart reception BEFORE any queue/network/classification work.
                    ready=listen()==RADIOLIB_ERR_NONE;
                    portENTER_CRITICAL(&lock); ++status.rx_packets; portEXIT_CRITICAL(&lock);
                    if (sequence==1) ESP_LOGI("radio","First real received packet: %u bytes RSSI %.1f dBm SNR %.1f dB",
                        e.packet.length,double(e.packet.rssi_dbm),double(e.packet.snr_db));
                    emit(e);
                } else {
                    if (rc==RADIOLIB_ERR_CRC_MISMATCH) { portENTER_CRITICAL(&lock); ++status.crc_errors; portEXIT_CRITICAL(&lock); }
                    ready=listen()==RADIOLIB_ERR_NONE;
                }
            }
        }
        sniffer::RadioConfig next;
        bool changed=xQueueReceive(commands,&next,0)==pdTRUE;
        if (changed) { config=next; index=0; }
        else if (config.mode==sniffer::Mode::Scan && now_ms()-switched>=config.dwell_ms) { index=(index+1)%config.profile_count; changed=true; }
        if (changed) {
            ready=configure()==RADIOLIB_ERR_NONE; switched=now_ms();
            auto e=radio_status(); e.type=sniffer::EventType::RadioConfig; emit(e);
        }
        if (!ready && now_ms()-last_error>=5000) {
            last_error=now_ms(); ready=configure(true)==RADIOLIB_ERR_NONE;
        }
        portENTER_CRITICAL(&lock); status.radio_ready=ready; portEXIT_CRITICAL(&lock);
    }
}
}
uint64_t now_ms() { return esp_timer_get_time()/1000; }
sniffer::Event radio_status() {
    portENTER_CRITICAL(&lock); auto e=status; portEXIT_CRITICAL(&lock);
    e.type=sniffer::EventType::Status; e.packet.timestamp_ms=now_ms();
    std::snprintf(e.boot_id,sizeof(e.boot_id),"%s",boot_id); return e;
}
void count_send_drop() { portENTER_CRITICAL(&lock); ++status.send_drops; portEXIT_CRITICAL(&lock); }
void radio_start() {
    telemetry=xQueueCreate(32,sizeof(sniffer::Event)); commands=xQueueCreate(1,sizeof(sniffer::RadioConfig));
    configASSERT(telemetry && commands);
    configASSERT(xTaskCreate(worker,"sx1262_rx",8192,nullptr,6,&task)==pdPASS);
}
}
