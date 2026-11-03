#include "lora/protocol.hpp"
#include "core/model.hpp"
#include "ui/lora_settings_model.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <random>
#include <type_traits>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while (0)
using namespace sniffer;
static void friendly_settings() {
    namespace friendly=dash::ui::lora_settings_model;
    uint32_t value=0; uint8_t sync=0;
    char formatted[24]; friendly::format_frequency(867500000,formatted,sizeof(formatted)); CHECK(!std::strcmp(formatted,"867.500"));
    friendly::format_frequency(867500001,formatted,sizeof(formatted)); CHECK(!std::strcmp(formatted,"867.500001"));
    CHECK(friendly::frequency_hz("867.500001",value) && value==867500001);
    CHECK(friendly::frequency_hz("869.525",value) && value==869525000);
    CHECK(friendly::frequency_hz("863",value) && value==863000000);
    CHECK(friendly::frequency_hz("870.000000",value) && value==870000000);
    for (const char* bad:{"", "862.9", "870.1", "867.5000001", "8.675e2", "NaN", "-867", "867,5"}) CHECK(!friendly::frequency_hz(bad,value));
    CHECK(friendly::duration_ms("0.25",value) && value==250);
    CHECK(friendly::duration_ms("2.345",value) && value==2345);
    CHECK(!friendly::duration_ms("0.0001",value));
    CHECK(friendly::sync_word("0x2B",sync) && sync==0x2B);
    CHECK(friendly::sync_word("12",sync) && sync==0x12);
    CHECK(friendly::sync_word("FF",sync) && sync==255);
    for (const char* bad:{"", "0x", "0x100", "xyz", "-1", " 12"}) CHECK(!friendly::sync_word(bad,sync));
    const auto preset=friendly::profile(friendly::Preset::MeshtasticLongFast);
    CHECK(valid(preset) && preset.frequency_hz==869525000 && preset.bw_hz==250000 && preset.sf==11 && preset.sync_word==0x2B && preset.preamble==16);
    CHECK(friendly::identify(preset)==friendly::Preset::MeshtasticLongFast);
    auto custom=preset; ++custom.frequency_hz; CHECK(friendly::identify(custom)==friendly::Preset::Custom);
}
static Event packet() {
    Event e; e.type=EventType::Packet; std::strcpy(e.boot_id,"0123456789abcdef");
    e.packet.sequence=1; e.packet.timestamp_ms=123456; e.packet.rssi_dbm=-99.5; e.packet.snr_db=4.25;
    e.packet.crc=Crc::Ok; e.packet.length=max_payload;
    for (size_t i=0;i<max_payload;++i) e.packet.payload[i]=uint8_t(i);
    return e;
}
static std::string wire(const Event& e) {
    char out[max_message+1]; const auto n=encode(e,out,sizeof(out)); CHECK(n>0 && n<=max_message); return {out,n};
}
static std::string replace(std::string s,const char* from,const char* to) {
    const auto at=s.find(from); CHECK(at!=std::string::npos); return s.replace(at,std::strlen(from),to);
}
static void protocol_tests() {
    Event e=packet(),out; auto json=wire(e);
    CHECK(decode(json.data(),json.size(),out)); CHECK(out.packet.length==max_payload);
    CHECK(out.packet.payload==e.packet.payload && out.packet.rssi_dbm==e.packet.rssi_dbm);
    CHECK(out.packet.profile.cr==5 && out.packet.crc==Crc::Ok);
    CHECK(out.packet.classification.confidence!=Confidence::Confirmed);
    for (auto type : {EventType::Status,EventType::Statistics,EventType::RadioConfig,EventType::ScanStatus,EventType::Cad,EventType::Error}) {
        e.type=type; e.rx_packets=1234; e.radio_ready=true; std::strcpy(e.message,"Authentication failed");
        const auto s=wire(e); CHECK(decode(s.data(),s.size(),out)); CHECK(out.type==type);
    }
    e=packet(); char tiny[8]; CHECK(encode(e,tiny,sizeof(tiny))==0);
    e.packet.crc=Crc::Unavailable; CHECK(!encode(e,tiny,sizeof(tiny)));
    for (const auto& bad : {
        replace(json,"\"schema\":1","\"schema\":2"),
        replace(json,"\"schema\":1","\"schema\":1,\"schema\":1"),
        replace(json,"\"schema\":1","\"schema\\u0000suffix\":1"),
        replace(json,"Custom 867.5","Custom 867.5\\u0000suffix"),
        replace(json,"\"packet\"","\"packet\\u0000\""),
        replace(json,"-99.50","null"), replace(json,"-99.50","1e999"),
        replace(json,"\"length\":255","\"length\":256"),
        replace(json,"\"sf\":8","\"sf\":13"),
        replace(json,"\"sf\":8","\"sf\":8.5"),
        replace(json,"\"crc\":\"ok\"","\"crc\":\"failed\""),
        replace(json,"\"payload_hex\":\"00","\"payload_hex\":\"G0"),
        json+"x",json.substr(0,json.size()-1),std::string(max_message+1,' '),std::string("{\"x\":")+std::string(30,'[')+"0"+std::string(30,']')+"}"})
        CHECK(!decode(bad.data(),bad.size(),out));
    for (size_t n=0;n<json.size();++n) CHECK(!decode(json.data(),n,out));
    auto nul=json; nul[10]=0; CHECK(!decode(nul.data(),nul.size(),out));
    RadioConfig c; c.profile_count=4; c.mode=Mode::Scan;
    for (size_t i=0;i<4;++i) c.profiles[i].frequency_hz+=uint32_t(i*100000);
    char buf[max_message+1]; auto n=encode_config(c,buf,sizeof(buf)); CHECK(n);
    RadioConfig parsed; CHECK(decode_config(buf,n,parsed)); CHECK(parsed.profile_count==4 && parsed.mode==Mode::Scan);
    CHECK(parsed.profiles[3].frequency_hz==867800000);
    const std::string config(buf,n);
    for (const auto& bad : {replace(config,"set_config","transmit"),replace(config,"\"schema\":1","\"schema\":1,\"tx\":true"),
        replace(config,"\"sf\":8","\"sf\":8,\"transmit\":true"),replace(config,"\"dwell_ms\":3000","\"dwell_ms\":0"),
        replace(config,"\"sf\":8","\"sf\":3"),replace(config,"867500000","900000000")}) CHECK(!decode_config(bad.data(),bad.size(),parsed));
    auto numeric=replace(config,"\"cr\":\"4/5\"","\"cr\":5"); CHECK(decode_config(numeric.data(),numeric.size(),parsed));
    const char token[]="0123456789abcdef0123456789abcdef";
    const std::string header=std::string("Bearer ")+token;
    CHECK(bearer_matches(header.data(),header.size(),token,sizeof(token)));
    for (size_t i=0;i<header.size();++i) { auto bad=header; bad[i]^=1; CHECK(!bearer_matches(bad.data(),bad.size(),token,sizeof(token))); }
    CHECK(!bearer_matches(header.data(),header.size()-1,token,sizeof(token)));
    CHECK(!bearer_matches(nullptr,0,token,sizeof(token)));
    CHECK(!safe_token("short",6,true)); CHECK(!safe_token("",1,true)); CHECK(safe_token("",1,false));
    ClientSettings s; CHECK(valid(s)); s.enabled=1; CHECK(!valid(s)); std::strcpy(s.token,token); CHECK(valid(s));
    std::strcpy(s.host,"example/path"); CHECK(!valid(s));
    const std::string provision=std::string("{\"type\":\"provision\",\"ssid\":\"LAN\",\"password\":\"private\",\"token\":\"")+token+"\"}";
    Provision credentials; CHECK(decode_provision(provision.data(),provision.size(),credentials)); CHECK(valid(credentials));
    for (const auto& bad : {replace(provision,"LAN","LAN\\u0000other"),provision+"x",
        replace(provision,"\"ssid\":\"LAN\"","\"ssid\":\"LAN\",\"ssid\":\"other\""),
        replace(provision,"\"type\":\"provision\"","\"type\":\"provision\",\"extra\":true")})
        CHECK(!decode_provision(bad.data(),bad.size(),credentials));
    CHECK(!decode_config(provision.data(),provision.size(),parsed)); // USB shape never becomes a WS command
}
static void usb_link_setup() {
    const std::string wire="{\"schema\":1,\"type\":\"provision_link\",\"host\":\"192.0.2.57\",\"port\":8080,\"token\":\"0123456789abcdef0123456789abcdef\",\"enabled\":true}";
    ClientSettings s; s.refresh_ms=750; s.history_size=17; s.radio.mode=Mode::Scan; s.radio.profile_count=3;
    s.radio.profiles[2].frequency_hz=867500001;
    const auto before=s;
    CHECK(decode_link_setup(wire.data(),wire.size(),s));
    CHECK(s.enabled && !std::strcmp(s.host,"192.0.2.57") && s.port==8080 && std::strlen(s.token)==32);
    CHECK(s.refresh_ms==before.refresh_ms && s.history_size==before.history_size &&
        !std::memcmp(&s.radio,&before.radio,sizeof(s.radio)));
    const auto configured=s;
    for (const auto& bad : {replace(wire,"\"schema\":1","\"schema\":2"),
        replace(wire,"192.0.2.57","192.0.2.57\\u0000other"),replace(wire,"\"port\":8080","\"port\":0"),
        replace(wire,"\"enabled\":true","\"enabled\":1"),
        replace(wire,"\"schema\":1","\"schema\":1,\"mode\":\"cad\""),
        replace(wire,"\"schema\":1","\"schema\":1,\"host\":\"other\""),wire+"junk"}) {
        CHECK(!decode_link_setup(bad.data(),bad.size(),s)); CHECK(!std::memcmp(&s,&configured,sizeof(s)));
    }
    RadioConfig radio; CHECK(!decode_config(wire.data(),wire.size(),radio));
    const std::string probe="{\"schema\":1,\"type\":\"sniffer_setup_probe\"}";
    CHECK(is_link_setup_probe(probe.data(),probe.size())); CHECK(!is_link_setup_probe(wire.data(),wire.size()));
    SetupLine line;
    for (char c:wire) CHECK(line.feed(uint8_t(c))==SetupLine::Result::More);
    CHECK(line.feed('\n')==SetupLine::Result::Complete && line.size()==wire.size());
    CHECK(decode_link_setup(line.data(),line.size(),s)); line.clear();
    for (unsigned i=0;i<700;++i) CHECK(line.feed('x')==SetupLine::Result::More);
    CHECK(line.feed('\n')==SetupLine::Result::Discarded);
    line.feed('{'); line.feed(0); line.feed('}'); CHECK(line.feed('\n')==SetupLine::Result::Discarded);
    for (char c:probe) line.feed(uint8_t(c));
    line.feed('\r'); CHECK(line.feed('\n')==SetupLine::Result::Complete);
    CHECK(is_link_setup_probe(line.data(),line.size()));
}
static void fragments() {
    const auto json=wire(packet()); MessageAssembler a;
    for (size_t chunk : {size_t(1),size_t(37),size_t(512),json.size()}) {
        a.reset();
        for (size_t offset=0;offset<json.size();offset+=chunk) {
            const size_t n=std::min(chunk,json.size()-offset);
            CHECK(a.feed(1,true,json.size(),offset,json.data()+offset,n)==(offset+n==json.size()));
            CHECK(!a.rejected());
        }
        CHECK(a.size()==json.size() && !std::memcmp(a.data(),json.data(),json.size()));
    }
    a.reset(); CHECK(!a.feed(1,false,4,0,"abcd",4));
    CHECK(!a.feed(9,true,1,0,"p",1)); CHECK(!a.rejected());
    CHECK(!a.feed(0,true,4,0,"ef",2)); CHECK(a.feed(0,true,4,2,"gh",2));
    CHECK(a.size()==8 && !std::memcmp(a.data(),"abcdefgh",8));
    // websockets15.send(iterable) terminates with an empty FIN continuation.
    a.reset(); CHECK(!a.feed(1,false,4,0,"abcd",4));
    CHECK(!a.feed(0,false,4,0,"efgh",4));
    CHECK(!a.feed(9,true,0,0,nullptr,0));
    CHECK(a.feed(0,true,0,0,nullptr,0)); CHECK(a.size()==8);
    a.reset(); CHECK(!a.feed(0,true,1,0,"x",1) && a.rejected());
    CHECK(!a.feed(2,true,1,0,"x",1) && a.rejected());
    CHECK(!a.feed(1,true,max_message+1,0,nullptr,0) && a.rejected());
    CHECK(!a.feed(1,true,4,0,"a",1)); CHECK(!a.feed(1,true,4,2,"bc",2) && a.rejected());
    CHECK(!a.feed(1,true,4,0,"a",1)); CHECK(!a.feed(0,true,4,1,"bcd",3) && a.rejected());
    CHECK(!a.feed(1,true,4,0,"a",1)); CHECK(!a.feed(1,true,1,0,"x",1) && a.rejected());
    CHECK(a.feed(1,true,0,0,nullptr,0)); // empty text is a message, but not JSON
    CHECK(!a.feed(1,true,4,0,"a",1)); CHECK(!a.feed(1,false,4,1,"bcd",3) && a.rejected());
}
static void classifiers() {
    RadioProfile p; std::array<uint8_t,255> b{};
    CHECK(classify(nullptr,10,p).protocol==Protocol::Unknown);
    CHECK(classify(b.data(),255,p).protocol==Protocol::Unknown);
    b[0]=0xff; b[4]=0x42; b[8]=1; p.sync_word=0x2b;
    CHECK(classify(b.data(),17,p).protocol==Protocol::Meshtastic);
    p.sync_word=0x12; CHECK(classify(b.data(),17,p).protocol==Protocol::Unknown);
    b={}; b[0]=1; b[2]=7; b[19]=42;
    CHECK(classify(b.data(),167,p).protocol==Protocol::Reticulum);
    b[0]|=0x80; CHECK(classify(b.data(),167,p).protocol==Protocol::Unknown);
    b={}; b[0]=0x11; b[2]=42;
    CHECK(classify(b.data(),103,p).protocol==Protocol::MeshCore);
    CHECK(classify(b.data(),102,p).protocol==Protocol::Unknown);
    b[4]=42; b[8]=1; p.sync_word=0x2b;
    CHECK(classify(b.data(),103,p).protocol==Protocol::Unknown); // ambiguous mesh headers
    p.sync_word=0x12;
    b={}; b[0]=0x10; b[5]=0x41; b[8]=42; // transport codes, one 2-byte path hash
    CHECK(classify(b.data(),109,p).protocol==Protocol::MeshCore);
    b[5]=0xc0; CHECK(classify(b.data(),109,p).protocol==Protocol::Unknown);
    std::mt19937 rng(43);
    for (unsigned i=0;i<20000;++i) {
        for (auto& byte:b) byte=uint8_t(rng());
        CHECK(classify(b.data(),rng()%256,p).confidence!=Confidence::Confirmed);
    }
}
static void history() {
    State s; auto e=packet(); CHECK(s.apply(e,1000,4)); CHECK(s.count()==1);
    CHECK(!s.apply(e,1100,4)); CHECK(s.count()==1);
    CHECK(std::abs(s.observed_rate(1100)-1.f/60)<.00001);
    for (uint64_t i=2;i<50;++i) { e.packet.sequence=i; CHECK(s.apply(e,1000+i*1000,4)); }
    CHECK(s.count()==4 && s.recent(0)->sequence==49 && s.recent(3)->sequence==46 && !s.recent(4));
    CHECK(s.observed_rate(200000)==0);
    e.type=EventType::Cad; e.cad_detected=true; CHECK(s.apply(e,200000,4));
    uint16_t packets[60],cad[60]; s.activity(200000,60,packets,cad,60); CHECK(packets[59]==0 && cad[59]==1);
    std::strcpy(e.boot_id,"fedcba9876543210"); e.type=EventType::Status; e.radio_ready=true;
    CHECK(s.apply(e,201000,4)); CHECK(s.count()==0 && s.radio_ready && std::isnan(s.last_rssi));
    s.activity(201000,900,packets,cad,60); for (auto n:cad) CHECK(n==0);
    e=packet(); for (uint64_t i=1;i<=900;++i) { e.packet.sequence=i; s.apply(e,i*1000,32); }
    CHECK(s.count()==32); CHECK(std::abs(s.observed_rate(900000)-1)<.00001);
    CHECK(s.observed_rate(1000000)==0);
}
// Frozen v1 layout: compare fields and serialized size with the original settings.
struct LegacySettings {
    uint32_t version; char ssid[33],wifi_password[65],mqtt_uri[160],mqtt_user[64],mqtt_password[96],device_id[32];
    uint32_t publish_seconds,refresh_ms,stale_ms; uint16_t screen_timeout_s,rotation_s; uint8_t brightness;
    dash::Thresholds limits; std::array<dash::HostConfig,4> hosts;
    char reticulum_url[160],reticulum_token[96],lora_topic[128],ntp_server[128];
};
static_assert(sizeof(LegacySettings)==sizeof(dash::CoreSettings));
static_assert(offsetof(LegacySettings,limits)==offsetof(dash::CoreSettings,limits));
static_assert(offsetof(LegacySettings,ntp_server)==offsetof(dash::CoreSettings,ntp_server));
static_assert(std::is_trivially_copyable<dash::CoreSettings>::value);
static void legacy() {
    dash::Settings old; std::strcpy(old.ssid,"Existing Wi-Fi"); std::strcpy(old.wifi_password,"unchanged");
    const dash::CoreSettings& base=old; std::array<uint8_t,sizeof(base)> blob; std::memcpy(blob.data(),&base,blob.size());
    dash::Settings restored; std::memcpy(static_cast<dash::CoreSettings*>(&restored),blob.data(),blob.size());
    CHECK(dash::validate_settings(restored)); CHECK(!std::strcmp(restored.ssid,old.ssid)); CHECK(!restored.sniffer.enabled);
}
static void fuzz() {
    std::mt19937 rng(44); Event out; RadioConfig config; MessageAssembler assembler;
    const auto original=wire(packet());
    for (unsigned i=0;i<10000;++i) {
        auto data=original; for (unsigned j=0;j<1+rng()%8;++j) data[rng()%data.size()]=char(rng());
        decode(data.data(),data.size(),out); decode_config(data.data(),data.size(),config);
        assembler.feed(uint8_t(rng()%16),rng()%2,rng()%4096,rng()%4096,data.data(),rng()%data.size());
    }
}
int main() {
    friendly_settings(); protocol_tests(); usb_link_setup(); fragments(); classifiers(); history(); legacy(); fuzz();
    std::puts("LoRa: wire schema, limits, malformed input, fragments, auth, classifiers, history, legacy NVS and deterministic fuzz passed");
}
