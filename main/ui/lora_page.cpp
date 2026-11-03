#include "lora_page.hpp"
#include "widgets.hpp"
#include "app/platform.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace dash::ui::lora_page {
namespace {
using namespace widgets;
ViewModel* vm;
lv_obj_t *frequency,*profile_label,*mode_label,*rssi,*snr,*count,*activity_label,*diagnostic;
lv_obj_t *bars,*rssi_chart,*snr_chart,*rows[sniffer::max_packets],*row_labels[sniffer::max_packets];
lv_chart_series_t *packet_series,*cad_series,*rssi_series,*snr_series;
uint32_t window=60;
uint64_t rendered=0;
sniffer::Packet selected;
void range(lv_event_t* e) { window=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)); rendered=0; refresh(platform::now_ms()); }
void statistics(lv_event_t*) {
    auto* modal=detail("Receiver counters"); if (!modal) return;
    const auto& s=vm->lora; char text[600];
    std::snprintf(text,sizeof(text),"Received %llu / CRC failures %llu\nAcquisition queue drops %llu\nSend-job / socket drops %llu\nD1S queue drops %llu / malformed %llu\nCAD detections %llu / checks %llu\n\nDrops count local pipeline loss, not all over-air loss. Rate counts events observed by this D1S in the last 60 seconds.",
        (unsigned long long)s.rx_packets,(unsigned long long)s.crc_errors,(unsigned long long)s.queue_drops,
        (unsigned long long)s.send_drops,(unsigned long long)s.local_drops,(unsigned long long)s.malformed,
        (unsigned long long)s.cad_detections,(unsigned long long)s.cad_checks);
    auto* note=label(modal,text,18,70,&lv_font_montserrat_16,white);
    lv_obj_set_width(note,416); lv_label_set_long_mode(note,LV_LABEL_LONG_WRAP);
}
void packet_detail(lv_event_t* e) {
    const size_t index=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    const auto* packet=vm->lora.recent(index); if (!packet) return;
    selected=*packet; auto* modal=detail("Received packet"); if (!modal) return;
    auto* body=make_panel(modal,12,62,432,280); lv_obj_add_flag(body,LV_OBJ_FLAG_SCROLLABLE);
    char text[1400],hex[sniffer::max_payload*2+1]; const char* digits="0123456789ABCDEF";
    for (size_t i=0;i<selected.length;++i) { hex[2*i]=digits[selected.payload[i]>>4]; hex[2*i+1]=digits[selected.payload[i]&15]; }
    hex[2*selected.length]=0;
    const auto& p=selected.profile;
    std::snprintf(text,sizeof(text),
        "Sniffer uptime %lluh %llum %llus\nSequence %llu / %u bytes\nProfile: %s\n%.3f MHz / SF%u / BW %.2f kHz\nCR4/%u / sync 0x%02X / preamble %u\nCRC: %s / explicit header\nRSSI %.1f dBm / SNR %.1f dB\nClassification: %s\nConfidence: %s (not authenticated)\nRaw HEX (no decryption):\n%s",
        (unsigned long long)(selected.timestamp_ms/3600000),(unsigned long long)(selected.timestamp_ms/60000%60),
        (unsigned long long)(selected.timestamp_ms/1000%60),(unsigned long long)selected.sequence,selected.length,p.name,
        p.frequency_hz/1e6,p.sf,p.bw_hz/1000.,p.cr,p.sync_word,p.preamble,
        selected.crc==sniffer::Crc::Ok?"ok":"disabled",double(selected.rssi_dbm),double(selected.snr_db),
        sniffer::protocol_name(selected.classification.protocol),selected.classification.confidence==sniffer::Confidence::Probable?"Probable":"Unknown",hex);
    auto* label_obj=label(body,text,12,12,&lv_font_montserrat_16,white);
    lv_obj_set_width(label_obj,404); lv_label_set_long_mode(label_obj,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label_obj,5,0);
}
}
void create(lv_obj_t* parent,ViewModel& model) {
    vm=&model; rendered=0;
    auto* p=make_panel(parent,18,78,444,99);
    frequency=label(p,"-- MHz",14,12,&lv_font_montserrat_28,cyan);
    mode_label=label(p,"NO RADIO SOURCE",265,18,&lv_font_montserrat_14,muted);
    lv_obj_set_width(mode_label,165); lv_label_set_long_mode(mode_label,LV_LABEL_LONG_DOT);
    profile_label=label(p,"Waiting for sniffer configuration",14,56,&lv_font_montserrat_16,muted);
    lv_obj_set_width(profile_label,416); lv_label_set_long_mode(profile_label,LV_LABEL_LONG_DOT);
    for (unsigned i=0;i<3;++i) {
        p=make_panel(parent,18+i*152,187,140,69);
        label(p,i==0?"RSSI dBm":i==1?"SNR dB":"RECEIVED",12,9,&lv_font_montserrat_14,muted);
        auto* value=label(p,"--",12,32,&lv_font_montserrat_24,i==2?green:cyan);
        if (i==0) { rssi=value; rssi_chart=line_chart(p,102,35,27,20,&rssi_series,cyan,24); }
        if (i==1) { snr=value; snr_chart=line_chart(p,102,35,27,20,&snr_series,green,24); }
        if (i==2) { count=value; lv_obj_add_flag(p,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(p,statistics,LV_EVENT_CLICKED,nullptr); }
    }
    activity_label=label(parent,"ACTIVITY / NO EVENTS",20,271,&lv_font_montserrat_14,muted);
    for (unsigned i=0;i<3;++i) {
        const uint32_t periods[]={60,300,900};
        action(parent,i==0?"60s":i==1?"5m":"15m",292+i*57,264,52,32,range,reinterpret_cast<void*>(uintptr_t(periods[i])));
    }
    bars=lv_chart_create(parent); lv_obj_remove_style_all(bars); lv_obj_set_pos(bars,22,304); lv_obj_set_size(bars,436,29);
    lv_chart_set_type(bars,LV_CHART_TYPE_BAR); lv_chart_set_point_count(bars,60); lv_chart_set_div_line_count(bars,0,0);
    lv_obj_set_style_pad_column(bars,1,LV_PART_ITEMS);
    packet_series=lv_chart_add_series(bars,lv_color_hex(cyan),LV_CHART_AXIS_PRIMARY_Y);
    cad_series=lv_chart_add_series(bars,lv_color_hex(amber),LV_CHART_AXIS_PRIMARY_Y);
    diagnostic=label(parent,"PACKETS cyan / CAD amber",20,340,&lv_font_montserrat_14,muted);
    lv_obj_set_width(diagnostic,440); lv_label_set_long_mode(diagnostic,LV_LABEL_LONG_DOT);
    auto* list=make_panel(parent,18,363,444,53); lv_obj_add_flag(list,LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i=0;i<sniffer::max_packets;++i) {
        rows[i]=make_panel(list,0,int(i*32),440,31); lv_obj_set_style_radius(rows[i],5,0);
        lv_obj_add_event_cb(rows[i],packet_detail,LV_EVENT_CLICKED,reinterpret_cast<void*>(i));
        row_labels[i]=label(rows[i],"",10,7,&lv_font_montserrat_14,white);
        lv_obj_set_width(row_labels[i],418); lv_label_set_long_mode(row_labels[i],LV_LABEL_LONG_DOT);
        lv_obj_add_flag(rows[i],LV_OBJ_FLAG_HIDDEN);
    }
    // Resolve widths before the first data update: LV_LABEL_LONG_DOT otherwise
    // truncates new labels against their still-unresolved coordinates.
    lv_obj_update_layout(parent);
}
void refresh(uint64_t now) {
    if (rendered && now>=rendered && now-rendered<vm->settings.sniffer.refresh_ms) return;
    rendered=now; const auto& s=vm->lora;
    const bool fresh=s.connected && s.last_message_ms && now>=s.last_message_ms && now-s.last_message_ms<=vm->settings.sniffer.timeout_ms;
    char text[192];
    if (fresh) std::snprintf(text,sizeof(text),"%.3f MHz",s.active.frequency_hz/1e6); else std::strcpy(text,"-- MHz");
    label_update(frequency,text);
    if (fresh) std::snprintf(text,sizeof(text),"%s / P%u",sniffer::mode_name(s.mode),s.active_index+1); else std::strcpy(text,"NO RADIO SOURCE");
    label_update(mode_label,text);
    if (fresh) std::snprintf(text,sizeof(text),"SF%u / BW %.2fk / CR4/%u / %s",s.active.sf,s.active.bw_hz/1000.,s.active.cr,s.active.name);
    else std::snprintf(text,sizeof(text),"%s",s.diagnostic);
    label_update(profile_label,text);
    if (std::isfinite(s.last_rssi)) std::snprintf(text,sizeof(text),"%.0f",double(s.last_rssi)); else std::strcpy(text,"--"); label_update(rssi,text);
    if (std::isfinite(s.last_snr)) std::snprintf(text,sizeof(text),"%.1f",double(s.last_snr)); else std::strcpy(text,"--"); label_update(snr,text);
    if (fresh) std::snprintf(text,sizeof(text),"%llu",(unsigned long long)s.rx_packets); else std::strcpy(text,"--"); label_update(count,text);
    uint16_t packets[60],cad[60]; s.activity(now,window,packets,cad,60); unsigned maximum=1;
    for (unsigned i=0;i<60;++i) { packet_series->y_points[i]=packets[i]; cad_series->y_points[i]=cad[i]; maximum=std::max(maximum,unsigned(std::max(packets[i],cad[i]))); }
    lv_chart_set_range(bars,LV_CHART_AXIS_PRIMARY_Y,0,maximum+1); lv_chart_refresh(bars);
    std::snprintf(text,sizeof(text),"OBSERVED %.2f pkt/s",double(s.observed_rate(now))); label_update(activity_label,text);
    if (s.local_drops || s.queue_drops || s.send_drops || s.malformed)
        std::snprintf(text,sizeof(text),"Drop UI:%llu queue:%llu send:%llu bad:%llu",(unsigned long long)s.local_drops,
            (unsigned long long)s.queue_drops,(unsigned long long)s.send_drops,(unsigned long long)s.malformed);
    else if (s.mode==sniffer::Mode::Cad) std::snprintf(text,sizeof(text),"CAD %llu/%llu / not decoded packets",(unsigned long long)s.cad_detections,(unsigned long long)s.cad_checks);
    else std::strcpy(text,"PACKETS cyan / CAD amber / uptime timestamps");
    label_update(diagnostic,text);
    float rssis[24],snrs[24]; std::fill(rssis,rssis+24,NAN); std::fill(snrs,snrs+24,NAN);
    for (size_t i=0;i<sniffer::max_packets;++i) {
        const auto* p=s.recent(i);
        if (!p) { lv_obj_add_flag(rows[i],LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(rows[i],LV_OBJ_FLAG_HIDDEN);
        const uint64_t sec=p->timestamp_ms/1000;
        std::snprintf(text,sizeof(text),"%02llu:%02llu:%02llu   %-11s   %.0f dBm / %.1f dB",
            (unsigned long long)(sec/3600),(unsigned long long)(sec/60%60),(unsigned long long)(sec%60),
            sniffer::protocol_name(p->classification.protocol),double(p->rssi_dbm),double(p->snr_db));
        label_update(row_labels[i],text);
        if (i<24) { rssis[23-i]=p->rssi_dbm; snrs[23-i]=p->snr_db; }
    }
    set_values(rssi_chart,rssi_series,rssis,24,25); set_values(snr_chart,snr_series,snrs,24,10);
}
}
