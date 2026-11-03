#pragma once
#include "lora/protocol.hpp"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace dash::ui::lora_settings_model {
enum class Preset { Custom, Default867, MeshtasticLongFast };
inline void compact_decimal(char* text,size_t minimum_fraction=0) {
    if (!text) return;
    const char* point=std::strchr(text,'.'); if (!point) return;
    size_t length=std::strlen(text),minimum=size_t(point-text)+1+minimum_fraction;
    while (length>minimum && text[length-1]=='0') text[--length]=0;
    if (!minimum_fraction && length && text[length-1]=='.') text[length-1]=0;
}
inline void format_frequency(uint32_t hz,char* text,size_t capacity) {
    if (!text || !capacity) return;
    std::snprintf(text,capacity,"%.6f",hz/1000000.0); compact_decimal(text,3);
}
inline bool decimal(const char* text,double& value) {
    if (!text || !*text) return false;
    // UI values are plain decimals, never exponent/locale/hex expressions.
    bool digit=false,point=false;
    for (const char* p=text;*p;++p) {
        if (*p>='0' && *p<='9') digit=true;
        else if (*p=='.' && !point) point=true;
        else return false;
    }
    char* end; value=std::strtod(text,&end);
    return digit && !*end && std::isfinite(value);
}
inline bool frequency_hz(const char* text,uint32_t& output) {
    double mhz;
    if (!decimal(text,mhz) || mhz<863 || mhz>870) return false;
    const double hz=mhz*1000000.0,rounded=std::round(hz);
    if (std::abs(hz-rounded)>.001) return false;
    output=uint32_t(rounded); return true;
}
inline bool duration_ms(const char* text,uint32_t& output) {
    double seconds;
    if (!decimal(text,seconds) || seconds<0 || seconds>3600) return false;
    const double ms=seconds*1000.0,rounded=std::round(ms);
    if (std::abs(ms-rounded)>.00001) return false;
    output=uint32_t(rounded); return true;
}
inline bool sync_word(const char* text,uint8_t& output) {
    if (!text) return false;
    if (text[0]=='0' && (text[1]=='x' || text[1]=='X')) text+=2;
    const size_t n=std::strlen(text);
    if (!n || n>2) return false;
    for (size_t i=0;i<n;++i)
        if (!((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f') || (text[i]>='A' && text[i]<='F'))) return false;
    output=uint8_t(std::strtoul(text,nullptr,16)); return true;
}
inline sniffer::RadioProfile profile(Preset preset) {
    sniffer::RadioProfile p;
    if (preset==Preset::MeshtasticLongFast) {
        std::strcpy(p.name,"Meshtastic EU868 LF");
        p.frequency_hz=869525000; p.bw_hz=250000; p.sf=11;
        p.sync_word=0x2B; p.preamble=16;
    }
    return p;
}
inline Preset identify(const sniffer::RadioProfile& p) {
    for (auto preset:{Preset::Default867,Preset::MeshtasticLongFast}) {
        const auto other=profile(preset);
        if (p.frequency_hz==other.frequency_hz && p.bw_hz==other.bw_hz && p.sf==other.sf &&
            p.cr==other.cr && p.sync_word==other.sync_word && p.preamble==other.preamble && p.crc_enabled==other.crc_enabled) return preset;
    }
    return Preset::Custom;
}
}
