#include "protocol.hpp"
#include "cJSON.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace sniffer {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
bool clean_tree(const cJSON* j, unsigned depth = 0, size_t* visited = nullptr) {
    size_t local = 0; if (!visited) visited = &local;
    if (depth > 8 || ++*visited > 192) return false;
    for (auto* a = j->child; a; a = a->next) {
        if (cJSON_IsObject(j)) {
            if (!a->string) return false;
            for (auto* b = a->next; b; b = b->next) if (b->string && !std::strcmp(a->string, b->string)) return false;
        }
        if (!clean_tree(a, depth + 1, visited)) return false;
    }
    return true;
}
Json parse(const char* text, size_t n) {
    Json empty(nullptr, cJSON_Delete);
    if (!text || !n || n > max_message || std::memchr(text, 0, n)) return empty;
    // Preflight depth before recursive cJSON allocation/parsing.
    unsigned depth = 0; bool quoted = false, escaped = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') {
                // cJSON strings are NUL terminated: reject embedded decoded NUL,
                // including in keys, rather than authenticating truncated strings.
                if (i+5<n && !std::memcmp(text+i,"\\u0000",6)) return empty;
                escaped = true;
            } else if (c == '"') quoted = false;
        }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 8) return empty; }
        else if (c == '}' || c == ']') { if (!depth) return empty; --depth; }
    }
    if (quoted || depth) return empty;
    const char* end = nullptr;
    Json root(cJSON_ParseWithLengthOpts(text, n, &end, false), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()) || !clean_tree(root.get())) return empty;
    while (end < text + n && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    if (end != text + n) return empty;
    return root;
}
const cJSON* item(const cJSON* j, const char* key) { return cJSON_GetObjectItemCaseSensitive(j, key); }
bool integer(const cJSON* j, const char* key, uint64_t lo, uint64_t hi, uint64_t& v) {
    const auto* n = item(j, key);
    if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) || n->valuedouble < double(lo) ||
        n->valuedouble > double(hi) || n->valuedouble != std::floor(n->valuedouble)) return false;
    v = uint64_t(n->valuedouble); return true;
}
template<typename T> bool number(const cJSON* j, const char* key, uint64_t lo, uint64_t hi, T& v) {
    uint64_t n; if (!integer(j, key, lo, hi, n)) return false; v = T(n); return true;
}
bool boolean(const cJSON* j, const char* key, bool& out) {
    const auto* b = item(j, key); if (!cJSON_IsBool(b)) return false; out = cJSON_IsTrue(b); return true;
}
bool real(const cJSON* j, const char* key, float lo, float hi, float& v) {
    const auto* n = item(j, key);
    if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) || n->valuedouble < lo || n->valuedouble > hi) return false;
    v = float(n->valuedouble); return true;
}
bool string(const cJSON* j, const char* key, char* out, size_t cap) {
    const auto* s = item(j, key);
    if (!cJSON_IsString(s) || !s->valuestring || std::strlen(s->valuestring) >= cap) return false;
    std::strcpy(out, s->valuestring); return true;
}
bool safe_name(const char* s, size_t cap) {
    if (!std::memchr(s, 0, cap) || !s[0]) return false;
    for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == ' ' || *s == '-' || *s == '_' || *s == '.')) return false;
    return true;
}
bool coding_rate(const cJSON* j,uint8_t& cr) {
    const auto* n=item(j,"cr");
    if (cJSON_IsString(n) && std::strlen(n->valuestring)==3 && n->valuestring[0]=='4' && n->valuestring[1]=='/' &&
        n->valuestring[2]>='5' && n->valuestring[2]<='8') { cr=n->valuestring[2]-'0'; return true; }
    return number(j,"cr",5,8,cr); // schema1 accepts numeric denominators as well
}
bool profile(const cJSON* j, RadioProfile& p) {
    bool crc;
    return string(j, "receiver_profile", p.name, sizeof(p.name)) &&
        number(j, "frequency_hz", 863000000, 870000000, p.frequency_hz) &&
        number(j, "bw_hz", 7810, 500000, p.bw_hz) && number(j, "sf", 5, 12, p.sf) &&
        coding_rate(j,p.cr) && number(j, "sync_word", 0, 255, p.sync_word) &&
        number(j, "preamble", 4, 65535, p.preamble) && boolean(j, "crc_enabled", crc) &&
        (p.crc_enabled = crc, valid(p));
}
size_t profile_json(const RadioProfile& p, char* out, size_t n) {
    const int len = std::snprintf(out, n,
        "\"receiver_profile\":\"%s\",\"frequency_hz\":%lu,\"bw_hz\":%lu,\"sf\":%u,\"cr\":\"4/%u\",\"sync_word\":%u,\"preamble\":%u,\"crc_enabled\":%s",
        p.name, (unsigned long)p.frequency_hz, (unsigned long)p.bw_hz, p.sf, p.cr, p.sync_word, p.preamble, p.crc_enabled ? "true" : "false");
    return len >= 0 && size_t(len) < n ? size_t(len) : 0;
}
bool read_mode(const cJSON* j, Mode& mode) {
    char name[12]; if (!string(j, "mode", name, sizeof(name))) return false;
    if (!std::strcmp(name, "fixed")) mode = Mode::Fixed;
    else if (!std::strcmp(name, "scan")) mode = Mode::Scan;
    else if (!std::strcmp(name, "cad")) mode = Mode::Cad;
    else return false;
    return true;
}
int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}
bool valid(const RadioProfile& p) {
    const uint32_t bandwidths[] = {7810,10420,15630,20830,31250,41670,62500,125000,250000,500000};
    return safe_name(p.name, sizeof(p.name)) && p.frequency_hz >= 863000000 && p.frequency_hz <= 870000000 &&
        std::find(std::begin(bandwidths), std::end(bandwidths), p.bw_hz) != std::end(bandwidths) &&
        p.sf >= 5 && p.sf <= 12 && p.cr >= 5 && p.cr <= 8 && p.preamble >= 4 && p.crc_enabled <= 1;
}
bool valid(const RadioConfig& c) {
    if (c.mode > Mode::Cad || !c.profile_count || c.profile_count > max_profiles || c.dwell_ms < 250 || c.dwell_ms > 60000) return false;
    for (size_t i = 0; i < c.profile_count; ++i) if (!valid(c.profiles[i])) return false;
    return true;
}
bool safe_token(const char* s, size_t cap, bool required) {
    if (!s || !std::memchr(s, 0, cap)) return false;
    const size_t n = std::strlen(s); if (!n) return !required;
    if (n < 32) return false;
    for (size_t i = 0; i < n; ++i) if (uint8_t(s[i]) < 33 || uint8_t(s[i]) > 126) return false;
    return true;
}
bool bearer_matches(const char* header,size_t n,const char* token,size_t cap) {
    if (!header || n>=128 || cap>96 || !safe_token(token,cap,true)) return false;
    char expected[128]{};
    const auto length=std::snprintf(expected,sizeof(expected),"Bearer %s",token);
    unsigned difference=n!=size_t(length);
    for (size_t i=0;i<sizeof(expected);++i) difference|=uint8_t(i<n?header[i]:0)^uint8_t(expected[i]);
    return difference==0;
}
bool valid(const Provision& p) {
    return p.version==1 && std::memchr(p.ssid,0,sizeof(p.ssid)) && p.ssid[0] &&
        std::memchr(p.password,0,sizeof(p.password)) && safe_token(p.token,sizeof(p.token),true);
}
bool decode_provision(const char* text,size_t n,Provision& output) {
    if (n>511) return false;
    auto root=parse(text,n); if (!root) return false;
    Provision p; char type[16];
    if (!string(root.get(),"type",type,sizeof(type)) || std::strcmp(type,"provision") ||
        !string(root.get(),"ssid",p.ssid,sizeof(p.ssid)) || !string(root.get(),"password",p.password,sizeof(p.password)) ||
        !string(root.get(),"token",p.token,sizeof(p.token)) || !valid(p)) return false;
    unsigned fields=0; for (auto* f=root->child; f; f=f->next) ++fields;
    if (fields!=4) return false;
    output=p; return true;
}
bool is_link_setup_probe(const char* text,size_t n) {
    if (n>511) return false;
    auto root=parse(text,n); if (!root) return false;
    uint32_t schema; char type[24]; unsigned fields=0;
    for (auto* f=root->child; f; f=f->next) ++fields;
    return fields==2 && number(root.get(),"schema",1,1,schema) &&
        string(root.get(),"type",type,sizeof(type)) && !std::strcmp(type,"sniffer_setup_probe");
}
bool decode_link_setup(const char* text,size_t n,ClientSettings& output) {
    if (n>511) return false;
    auto root=parse(text,n); if (!root) return false;
    ClientSettings candidate=output; uint32_t schema; char type[24]; bool enabled;
    if (!number(root.get(),"schema",1,1,schema) || !string(root.get(),"type",type,sizeof(type)) ||
        std::strcmp(type,"provision_link") || !string(root.get(),"host",candidate.host,sizeof(candidate.host)) ||
        !string(root.get(),"token",candidate.token,sizeof(candidate.token)) ||
        !number(root.get(),"port",1,65535,candidate.port) || !boolean(root.get(),"enabled",enabled)) return false;
    unsigned fields=0; for (auto* f=root->child; f; f=f->next) ++fields;
    if (fields!=6) return false;
    candidate.enabled=enabled?1:0;
    if (!valid(candidate)) return false;
    output=candidate; return true;
}
SetupLine::Result SetupLine::feed(uint8_t byte) {
    if (byte=='\n') {
        size_=used_; used_=0;
        if (discard_) { discard_=false; size_=0; buffer_[0]=0; return Result::Discarded; }
        buffer_[size_]=0; return size_?Result::Complete:Result::More;
    }
    if (!byte || used_>=buffer_.size()-1) discard_=true;
    if (!discard_) buffer_[used_++]=char(byte);
    return Result::More;
}
void SetupLine::clear() { buffer_={}; used_=size_=0; discard_=false; }
bool valid(const ClientSettings& s) {
    if (!std::memchr(s.host, 0, sizeof(s.host)) || !s.host[0] || !s.port || s.history_size < 1 || s.history_size > max_packets ||
        s.timeout_ms < 2000 || s.timeout_ms > 60000 || s.refresh_ms < 200 || s.refresh_ms > 5000 ||
        s.enabled > 1 || s.apply_on_connect > 1 || !safe_token(s.token, sizeof(s.token), s.enabled) || !valid(s.radio)) return false;
    for (const char* p = s.host; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
    return true;
}
bool decode(const char* text, size_t n, Event& output) {
    auto root = parse(text, n); if (!root) return false;
    const cJSON* j = root.get(); uint32_t schema; char type[24]; Event e;
    if (!number(j, "schema", 1, 1, schema) || !string(j, "type", type, sizeof(type)) ||
        !string(j, "boot_id", e.boot_id, sizeof(e.boot_id)) || std::strlen(e.boot_id) != 16) return false;
    for (char c : e.boot_id) if (c && unhex(c) < 0) return false;
    if (!std::strcmp(type, "error")) {
        e.type = EventType::Error;
        if (!string(j, "message", e.message, sizeof(e.message))) return false;
        for (char* p = e.message; *p; ++p) if (uint8_t(*p) < 32 || uint8_t(*p) > 126) *p = '?';
        output = e; return true;
    }
    if (!read_mode(j, e.mode) || !number(j, "profile_index", 0, max_profiles - 1, e.active_profile) ||
        !profile(j, e.packet.profile) || !number(j, "timestamp_ms", 0, 9007199254740991ull, e.packet.timestamp_ms)) return false;
    if (!std::strcmp(type, "packet")) {
        e.type = EventType::Packet; char hex[max_payload * 2 + 1], crc[16];
        if (!number(j, "sequence", 1, 9007199254740991ull, e.packet.sequence) ||
            !number(j, "length", 0, max_payload, e.packet.length) ||
            !real(j, "rssi_dbm", -200, 20, e.packet.rssi_dbm) || !real(j, "snr_db", -40, 30, e.packet.snr_db) ||
            !string(j, "payload_hex", hex, sizeof(hex)) || std::strlen(hex) != e.packet.length * 2u ||
            !string(j, "crc", crc, sizeof(crc))) return false;
        if (!std::strcmp(crc, "ok") && e.packet.profile.crc_enabled) e.packet.crc = Crc::Ok;
        else if (!std::strcmp(crc, "disabled") && !e.packet.profile.crc_enabled) e.packet.crc = Crc::Disabled;
        else return false; // failed-CRC data is never called a successful packet
        for (size_t i = 0; i < e.packet.length; ++i) {
            const int a = unhex(hex[2*i]), b = unhex(hex[2*i+1]); if (a < 0 || b < 0) return false;
            e.packet.payload[i] = uint8_t(a * 16 + b);
        }
        e.packet.classification = classify(e.packet.payload.data(), e.packet.length, e.packet.profile);
    } else if (!std::strcmp(type, "cad")) {
        e.type = EventType::Cad; if (!boolean(j, "detected", e.cad_detected)) return false;
    } else {
        if (!std::strcmp(type, "status")) e.type = EventType::Status;
        else if (!std::strcmp(type, "radio_config")) e.type = EventType::RadioConfig;
        else if (!std::strcmp(type, "statistics")) e.type = EventType::Statistics;
        else if (!std::strcmp(type, "scan_status")) e.type = EventType::ScanStatus;
        else return false;
        if (!boolean(j, "radio_ready", e.radio_ready)) return false;
        uint64_t* counters[] = {&e.rx_packets,&e.crc_errors,&e.queue_drops,&e.send_drops,&e.cad_checks,&e.cad_detections};
        const char* keys[] = {"rx_packets","crc_errors","queue_drops","send_drops","cad_checks","cad_detections"};
        for (size_t i = 0; i < 6; ++i) if (!integer(j, keys[i], 0, 9007199254740991ull, *counters[i])) return false;
    }
    output = e; return true;
}
bool decode_config(const char* text, size_t n, RadioConfig& output) {
    auto root = parse(text, n); if (!root) return false;
    uint32_t schema; char type[24]; RadioConfig c;
    if (!number(root.get(), "schema", 1, 1, schema) || !string(root.get(), "type", type, sizeof(type)) || std::strcmp(type,"set_config") ||
        !read_mode(root.get(), c.mode) || !number(root.get(), "dwell_ms", 250, 60000, c.dwell_ms)) return false;
    const auto* profiles = item(root.get(), "profiles");
    if (!cJSON_IsArray(profiles) || cJSON_GetArraySize(profiles) < 1 || cJSON_GetArraySize(profiles) > int(max_profiles)) return false;
    // Unknown top-level command fields are rejected (in particular no transmit operation).
    for (auto* f = root->child; f; f = f->next)
        if (std::strcmp(f->string,"schema") && std::strcmp(f->string,"type") && std::strcmp(f->string,"mode") &&
            std::strcmp(f->string,"dwell_ms") && std::strcmp(f->string,"profiles")) return false;
    c.profile_count = cJSON_GetArraySize(profiles);
    for (size_t i = 0; i < c.profile_count; ++i) {
        const auto* p=cJSON_GetArrayItem(profiles,i);
        if (!profile(p,c.profiles[i])) return false;
        for (auto* f=p->child; f; f=f->next)
            if (std::strcmp(f->string,"receiver_profile") && std::strcmp(f->string,"frequency_hz") &&
                std::strcmp(f->string,"bw_hz") && std::strcmp(f->string,"sf") && std::strcmp(f->string,"cr") &&
                std::strcmp(f->string,"sync_word") && std::strcmp(f->string,"preamble") && std::strcmp(f->string,"crc_enabled")) return false;
    }
    if (!valid(c)) return false;
    output = c; return true;
}
size_t encode(const Event& e, char* output, size_t cap) {
    if (!output || !cap || !valid(e.packet.profile) || !std::memchr(e.boot_id,0,sizeof(e.boot_id)) || std::strlen(e.boot_id) != 16) return 0;
    for (size_t i=0;i<16;++i) if (unhex(e.boot_id[i])<0) return 0;
    if (e.mode>Mode::Cad || e.active_profile>=max_profiles || e.packet.timestamp_ms>9007199254740991ull) return 0;
    if (e.type == EventType::Error) {
        // Firmware errors are fixed identifiers, not echoed user input.
        if (!safe_name(e.message, sizeof(e.message))) return 0;
        const int n = std::snprintf(output, cap, "{\"schema\":1,\"type\":\"error\",\"boot_id\":\"%s\",\"message\":\"%s\"}", e.boot_id,e.message);
        return n > 0 && size_t(n) < cap ? size_t(n) : 0;
    }
    const char* types[] = {"packet","status","radio_config","statistics","error","scan_status","cad"};
    if (size_t(e.type) >= std::size(types)) return 0;
    char p[320]; if (!profile_json(e.packet.profile, p, sizeof(p))) return 0;
    const int start = std::snprintf(output, cap,
        "{\"schema\":1,\"type\":\"%s\",\"boot_id\":\"%s\",\"timestamp_ms\":%llu,\"mode\":\"%s\",\"profile_index\":%u,%s,",
        types[size_t(e.type)], e.boot_id, (unsigned long long)e.packet.timestamp_ms, mode_name(e.mode), e.active_profile, p);
    if (start < 0 || size_t(start) >= cap) return 0;
    int tail;
    if (e.type == EventType::Packet) {
        if (e.packet.length > max_payload || !e.packet.sequence || e.packet.sequence>9007199254740991ull ||
            !std::isfinite(e.packet.rssi_dbm) || e.packet.rssi_dbm < -200 || e.packet.rssi_dbm>20 ||
            !std::isfinite(e.packet.snr_db) || e.packet.snr_db < -40 || e.packet.snr_db>30 ||
            (e.packet.profile.crc_enabled ? e.packet.crc!=Crc::Ok : e.packet.crc!=Crc::Disabled)) return 0;
        char hex[max_payload * 2 + 1]; const char* digits = "0123456789ABCDEF";
        for (size_t i = 0; i < e.packet.length; ++i) { hex[2*i]=digits[e.packet.payload[i]>>4]; hex[2*i+1]=digits[e.packet.payload[i]&15]; }
        hex[e.packet.length*2] = 0;
        tail = std::snprintf(output+start,cap-start,
            "\"sequence\":%llu,\"rssi_dbm\":%.2f,\"snr_db\":%.2f,\"length\":%u,\"crc\":\"%s\",\"payload_hex\":\"%s\"}",
            (unsigned long long)e.packet.sequence,double(e.packet.rssi_dbm),double(e.packet.snr_db),e.packet.length,
            e.packet.crc==Crc::Ok?"ok":"disabled",hex);
    } else if (e.type == EventType::Cad) tail = std::snprintf(output+start,cap-start,"\"detected\":%s}",e.cad_detected?"true":"false");
    else tail = std::snprintf(output+start,cap-start,
        "\"radio_ready\":%s,\"rx_packets\":%llu,\"crc_errors\":%llu,\"queue_drops\":%llu,\"send_drops\":%llu,\"cad_checks\":%llu,\"cad_detections\":%llu}",
        e.radio_ready?"true":"false",(unsigned long long)e.rx_packets,(unsigned long long)e.crc_errors,
        (unsigned long long)e.queue_drops,(unsigned long long)e.send_drops,(unsigned long long)e.cad_checks,(unsigned long long)e.cad_detections);
    return tail >= 0 && size_t(start+tail) < cap ? size_t(start+tail) : 0;
}
size_t encode_config(const RadioConfig& c, char* out, size_t cap) {
    if (!valid(c) || !out || !cap) return 0;
    int n = std::snprintf(out,cap,"{\"schema\":1,\"type\":\"set_config\",\"mode\":\"%s\",\"dwell_ms\":%lu,\"profiles\":[",mode_name(c.mode),(unsigned long)c.dwell_ms);
    if (n < 0 || size_t(n) >= cap) return 0;
    size_t at = n;
    for (size_t i = 0; i < c.profile_count; ++i) {
        if (at + 4 >= cap) return 0;
        if (i) out[at++]=',';
        out[at++]='{';
        const size_t len = profile_json(c.profiles[i],out+at,cap-at); if (!len) return 0;
        at += len; if (at+3>=cap) return 0; out[at++]='}';
    }
    out[at++]=']'; out[at++]='}'; out[at]=0; return at;
}
void MessageAssembler::reset() { used_=0; frame_used_=0; expected_frame_=0; active_=false; complete_=false; frame_open_=false; rejected_=false; }
bool MessageAssembler::feed(uint8_t opcode,bool fin,size_t frame_length,size_t offset,const char* data,size_t n) {
    rejected_=false;
    if (opcode==8 || opcode==9 || opcode==10) return false; // control frames never overwrite a fragmented message
    if (complete_) reset();
    const auto reject=[this]() { reset(); rejected_=true; return false; };
    if (offset == 0) {
        if (frame_open_) return reject();
        if (opcode == 1) { if (active_) return reject(); active_=true; }
        else if (opcode != 0 || !active_) return reject();
        if (frame_length>max_message-used_) return reject();
        frame_used_=0; expected_frame_=frame_length; frame_opcode_=opcode; frame_fin_=fin; frame_open_=true;
    }
    if (!active_ || !frame_open_ || opcode!=frame_opcode_ || fin!=frame_fin_ || offset!=frame_used_ ||
        frame_length!=expected_frame_ || offset>frame_length || n>frame_length-offset ||
        n>max_message-used_ || (n && !data)) return reject();
    if (n) std::memcpy(buffer_.data()+used_,data,n);
    used_+=n; frame_used_+=n;
    if (frame_used_==frame_length) frame_open_=false;
    if (frame_used_==frame_length && fin) { buffer_[used_]=0; complete_=true; active_=false; return true; }
    return false;
}
bool State::apply(const Event& e,uint64_t now,size_t capacity) {
    if (std::strcmp(boot_id,e.boot_id)) {
        std::snprintf(boot_id,sizeof(boot_id),"%s",e.boot_id); bins_={}; head_=count_=0; last_sequence_=0;
        last_packet_ms=0; last_rssi=last_snr=NAN;
        radio_ready=false; rx_packets=crc_errors=queue_drops=send_drops=cad_checks=cad_detections=0;
    }
    last_message_ms=now; ++revision;
    if (e.type==EventType::Error) { std::snprintf(diagnostic,sizeof(diagnostic),"%s",e.message); return true; }
    mode=e.mode; active=e.packet.profile; active_index=e.active_profile;
    if (e.type==EventType::Packet || (e.type==EventType::Cad && e.cad_detected)) {
        auto& b=bins_[(now/1000)%bins_.size()];
        if (b.second_plus_one!=now/1000+1) { b={}; b.second_plus_one=now/1000+1; }
        if (e.type==EventType::Packet) {
            if (e.packet.sequence<=last_sequence_) return false;
            last_sequence_=e.packet.sequence; last_packet_ms=now; last_rssi=e.packet.rssi_dbm; last_snr=e.packet.snr_db;
            packets_[head_]=e.packet; head_=(head_+1)%max_packets;
            count_=std::min(count_+1,std::clamp(capacity,size_t(1),max_packets));
            if (b.packets<65535) ++b.packets;
        } else if (b.cad<65535) ++b.cad;
    } else if (e.type!=EventType::Cad) {
        radio_ready=e.radio_ready; rx_packets=e.rx_packets; crc_errors=e.crc_errors; queue_drops=e.queue_drops;
        send_drops=e.send_drops; cad_checks=e.cad_checks; cad_detections=e.cad_detections;
    }
    return true;
}
const Packet* State::recent(size_t i) const { return i<count_?&packets_[(head_+max_packets-1-i)%max_packets]:nullptr; }
void State::activity(uint64_t now,uint32_t seconds,uint16_t* p,uint16_t* c,size_t n) const {
    if (!n || !p || !c) return;
    std::fill(p,p+n,0); std::fill(c,c+n,0); if (!seconds || seconds>900) return;
    const uint64_t end=now/1000;
    for (const auto& b:bins_) {
        if (!b.second_plus_one || b.second_plus_one-1>end) continue;
        const uint64_t age=end-(b.second_plus_one-1); if (age>=seconds) continue;
        const size_t i=n-1-std::min(n-1,size_t(age*n/seconds));
        p[i]=uint16_t(std::min(65535u,unsigned(p[i])+b.packets)); c[i]=uint16_t(std::min(65535u,unsigned(c[i])+b.cad));
    }
}
float State::observed_rate(uint64_t now) const {
    uint16_t p[1],c[1]; activity(now,60,p,c,1); return p[0]/60.f;
}
} // namespace sniffer
