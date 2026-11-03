#include "ui/dashboard.hpp"
#include "app/platform.hpp"
#include "lora/client.hpp"
#include "lvgl.h"
#include <SDL.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <fstream>

static std::array<uint32_t, 480 * 480> pixels;
static std::array<lv_color_t, 480 * 40> draw_buffer;
static uint64_t clock_ms = 86400000;
static int pointer_x = 0, pointer_y = 0;
static bool pressed = false;
static uint8_t backlight=85;
static dash::ViewModel model;
namespace dash::platform {
uint64_t now_ms() { return clock_ms; }
bool save_settings(const Settings& s) { model.settings = s; return true; } // RAM only, no credentials on disk
void apply_backlight(uint8_t percent) { backlight=percent; }
}
namespace dash::lora {
bool apply_radio(const sniffer::RadioConfig& c) {
    if (!model.simulated || !sniffer::valid(c)) return false;
    sniffer::Event e; std::strcpy(e.boot_id,"0123456789abcdef");
    e.type=sniffer::EventType::RadioConfig; e.mode=c.mode; e.packet.profile=c.profiles[0]; e.radio_ready=true;
    model.lora.apply(e,clock_ms,model.settings.sniffer.history_size); return true;
}
}
static void simulate_lora(uint64_t at, uint64_t sequence) {
    sniffer::Event e; std::strcpy(e.boot_id,"0123456789abcdef");
    e.type=sniffer::EventType::Packet; e.packet.sequence=sequence; e.packet.timestamp_ms=at;
    e.packet.rssi_dbm=-95+12*std::sin(sequence*.7); e.packet.snr_db=4+3*std::sin(sequence*.4);
    e.packet.crc=sniffer::Crc::Ok; e.packet.length=sequence%10==0?sniffer::max_payload:12;
    for (size_t i=0;i<e.packet.length;++i) e.packet.payload[i]=uint8_t(sequence+i*17);
    // Exercise the exact wire encoder/parser, with arbitrary unknown payloads.
    char json[sniffer::max_message+1]; sniffer::Event decoded;
    const auto n=sniffer::encode(e,json,sizeof(json));
    if (!n || !sniffer::decode(json,n,decoded)) std::abort();
    model.lora.apply(decoded,at,model.settings.sniffer.history_size);
    e.type=sniffer::EventType::Status; e.radio_ready=true; e.rx_packets=sequence;
    model.lora.apply(e,at,model.settings.sniffer.history_size);
}
static void flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* p) {
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) pixels[y * 480 + x] = lv_color_to32(*p++);
    lv_disp_flush_ready(driver);
}
static void input(lv_indev_drv_t*, lv_indev_data_t* data) {
    data->point.x = pointer_x; data->point.y = pointer_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void simulate() {
    model.simulated = true;
    model.settings.sniffer.enabled=1;
    std::strcpy(model.settings.sniffer.token,"desktop-only-not-a-real-token-0000");
    model.lora.enabled=model.lora.connected=true;
    std::strcpy(model.lora.diagnostic,"SIMULATED / no radio or network I/O");
    for (unsigned i=1;i<=120;++i) simulate_lora(clock_ms-(120-i)*7000,i);
    for (unsigned minute = 0; minute < 1440; ++minute) {
        const uint64_t at = clock_ms - (1439 - minute) * 60000ull;
        const float wave = std::sin(minute * .015f), detail = std::sin(minute * .42f);
        const float values[] = {22.8f + wave * .6f + detail * .08f, 46.2f + wave * 3,
                                624 + wave * 90 + detail * 16, 82 + wave * 17 + detail * 4};
        for (size_t m = 0; m < 4; ++m) {
            model.history.add({dash::Metric(m), values[m], at});
            model.readings[m] = {values[m], at};
        }
    }
    auto& n = model.network;
    n.connected = true; n.rssi = -52; n.free_heap = 560 * 1024; n.uptime_ms = clock_ms;
    std::strcpy(n.ssid, "Studio / simulation"); std::strcpy(n.ip, "192.0.2.42");
    std::strcpy(n.gateway, "192.0.2.1"); std::strcpy(n.dns, "192.0.2.2");
    const char* names[] = {"Router", "Home Assistant", "Pi-hole", "Linux server"};
    for (size_t i = 0; i < 4; ++i) {
        std::snprintf(model.settings.hosts[i].name, 32, "%s", names[i]);
        std::snprintf(model.settings.hosts[i].address, 128, "192.0.2.%u", unsigned(i + 1));
        auto& h = n.hosts[i]; h.checked = true; h.online = i != 3;
        h.latency_ms = 3 + i * 5; h.last_ok_ms = clock_ms - 3000;
        h.count = 32;
        for (size_t j = 0; j < 32; ++j) h.history[j] = h.online ? 3 + i * 5 + 3 * std::abs(std::sin(j * .4f)) : NAN;
    }
}
static void write_ppm(const std::string& path) {
    std::ofstream out(path, std::ios::binary); out << "P6\n480 480\n255\n";
    for (uint32_t p : pixels) {
        const char rgb[] = {char(p >> 16), char(p >> 8), char(p)}; out.write(rgb, 3);
    }
    if (!out) { std::fprintf(stderr, "Could not save %s\n", path.c_str()); std::exit(1); }
}
static lv_obj_t* find_text(lv_obj_t* parent, const char* wanted) {
    if (lv_obj_check_type(parent, &lv_label_class) && !std::strcmp(lv_label_get_text(parent), wanted)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i)
        if (auto* found = find_text(lv_obj_get_child(parent, i), wanted)) return found;
    return nullptr;
}
static lv_obj_t* find_type(lv_obj_t* parent, const lv_obj_class_t* type) {
    if (lv_obj_check_type(parent, type)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i)
        if (auto* found = find_type(lv_obj_get_child(parent, i), type)) return found;
    return nullptr;
}
static lv_obj_t* find_prefix(lv_obj_t* parent, const char* prefix) {
    if (lv_obj_check_type(parent,&lv_label_class) && !std::strncmp(lv_label_get_text(parent),prefix,std::strlen(prefix))) return parent;
    for (uint32_t i=0;i<lv_obj_get_child_cnt(parent);++i)
        if (auto* found=find_prefix(lv_obj_get_child(parent,i),prefix)) return found;
    return nullptr;
}
static void click_label(const char* label) {
    auto* o = find_text(lv_scr_act(), label);
    if (!o) { std::fprintf(stderr, "UI test missing label: %s\n", label); std::exit(1); }
    auto* target=lv_obj_get_parent(o);
    lv_event_send(target, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_event_send(target, LV_EVENT_CLICKED, nullptr);
}
static void require(bool condition,const char* message) {
    if (!condition) { std::fprintf(stderr,"UI test: %s\n",message); std::exit(1); }
}
static lv_obj_t* find_input_text(lv_obj_t* parent,const char* wanted) {
    if (lv_obj_check_type(parent,&lv_textarea_class) && !std::strcmp(lv_textarea_get_text(parent),wanted)) return parent;
    for (uint32_t i=0;i<lv_obj_get_child_cnt(parent);++i)
        if (auto* found=find_input_text(lv_obj_get_child(parent,i),wanted)) return found;
    return nullptr;
}
static void touch_at(int x,int y,bool down,unsigned milliseconds) {
    pointer_x=x; pointer_y=y; pressed=down;
    for (unsigned elapsed=0;elapsed<milliseconds;elapsed+=10) { lv_tick_inc(10); lv_timer_handler(); }
}
static void check_modal_blocks_tabs(const char* title,const char* background="HOST HEALTH") {
    auto* label=find_text(lv_scr_act(),title); require(label,"expected modal title missing");
    auto* shield=lv_obj_get_parent(lv_obj_get_parent(label));
    require(shield!=lv_scr_act() && lv_obj_get_parent(shield)==lv_scr_act(),"modal has no input shield");
    require(lv_obj_get_width(shield)==480 && lv_obj_get_height(shield)==480,"shield must cover navigation");
    lv_refr_now(nullptr);
    std::array<uint32_t,480*54> before;
    std::copy(pixels.begin()+480*426,pixels.end(),before.begin());
    // Exercise LVGL hit testing/pressed styles, not synthetic event dispatch.
    touch_at(64,452,true,80); lv_refr_now(nullptr);
    require(std::equal(before.begin(),before.end(),pixels.begin()+480*426),"background tabs changed while modal was pressed");
    auto* climate=find_text(lv_scr_act(),"Climate");
    require(climate && !lv_obj_has_state(lv_obj_get_parent(climate),LV_STATE_PRESSED),"background tab acquired pressed state");
    touch_at(64,452,false,80);
    require(find_text(lv_scr_act(),title) && find_text(lv_scr_act(),background),"tap navigated through modal");
}
static lv_obj_t* named_control(const char* caption,const lv_obj_class_t* type) {
    auto* label=find_text(lv_scr_act(),caption); require(label,"control caption missing");
    auto* parent=lv_obj_get_parent(label);
    for (uint32_t i=0;i+1<lv_obj_get_child_cnt(parent);++i)
        if (lv_obj_get_child(parent,i)==label) {
            auto* control=lv_obj_get_child(parent,i+1);
            require(lv_obj_check_type(control,type),"wrong control type after caption"); return control;
        }
    require(false,"control missing after caption"); return nullptr;
}
static void choose(const char* caption,unsigned index) {
    auto* control=named_control(caption,&lv_dropdown_class);
    require(index<lv_dropdown_get_option_cnt(control),"dropdown index out of range");
    lv_dropdown_set_selected(control,index); lv_event_send(control,LV_EVENT_VALUE_CHANGED,nullptr);
}
static void lora_settings_tests() {
    const auto original=model.settings; const auto original_state=model.lora;
    dash::ui::navigate(3); click_label(LV_SYMBOL_SETTINGS);
    require(find_text(lv_scr_act(),"LoRa settings") && find_text(lv_scr_act(),"Connection"),"LoRa setup sections missing");
    require(!find_text(lv_scr_act(),"P1") && !find_text(lv_scr_act(),"Enabled (0/1)"),"legacy coded settings still visible");
    check_modal_blocks_tabs("LoRa settings","RSSI dBm");
    auto* token=named_control("Access token",&lv_textarea_class);
    require(lv_textarea_get_password_mode(token),"access token is not masked");
    lv_textarea_set_text(named_control("XIAO address (hostname or IP)",&lv_textarea_class),"192.0.2.77");
    click_label("Receiver");
    require(lv_obj_has_state(lv_obj_get_parent(find_text(lv_scr_act(),"Apply radio")),LV_STATE_DISABLED),"live apply allowed with changed connection");
    touch_at(410,164,true,80); touch_at(410,164,false,80);
    require(lv_dropdown_is_open(named_control("Listen mode",&lv_dropdown_class)),"mode dropdown did not open from pointer input");
    check_modal_blocks_tabs("LoRa settings","RSSI dBm");
    lv_dropdown_close(named_control("Listen mode",&lv_dropdown_class));
    choose("Tuning preset",2);
    require(find_prefix(lv_scr_act(),"869.525 MHz"),"preset summary did not update");
    require(model.settings.sniffer.radio.profiles[0].frequency_hz==original.sniffer.radio.profiles[0].frequency_hz,"preset selection saved unexpectedly");
    auto* restore=named_control("Restore radio on reconnect",&lv_switch_class);
    lv_obj_add_state(restore,LV_STATE_CHECKED); lv_event_send(restore,LV_EVENT_VALUE_CHANGED,nullptr);
    choose("Listen mode",1); choose("Scan size",2); choose("Edit profile",2); choose("Tuning preset",0);
    auto* frequency=named_control("Frequency (863-870 MHz)",&lv_textarea_class);
    lv_textarea_set_text(frequency,"867.500001");
    lv_textarea_set_text(named_control("Sync word (hex, e.g. 0x12 or 0x2B)",&lv_textarea_class),"0x2B");
    choose("Bandwidth",8); choose("Spreading factor",4); choose("Coding rate",2);
    choose("Edit profile",0); choose("Edit profile",2);
    require(find_input_text(lv_scr_act(),"867.500001") && find_input_text(lv_scr_act(),"0x2B"),"profile switching lost tuning edits");
    require(lv_dropdown_get_selected(named_control("Spreading factor",&lv_dropdown_class))==4,"SF mapping lost");
    frequency=named_control("Frequency (863-870 MHz)",&lv_textarea_class); lv_textarea_set_text(frequency,"900");
    choose("Edit profile",0);
    require(lv_dropdown_get_selected(named_control("Edit profile",&lv_dropdown_class))==2,"invalid frequency was silently discarded");
    lv_textarea_set_text(frequency,"867.500001");
    click_label("Connection"); require(find_input_text(lv_scr_act(),"192.0.2.77"),"section switching lost address draft");
    auto* enable=named_control("Connect to XIAO",&lv_switch_class);
    lv_obj_add_state(enable,LV_STATE_CHECKED); lv_event_send(enable,LV_EVENT_VALUE_CHANGED,nullptr);
    token=named_control("Access token",&lv_textarea_class); lv_textarea_set_text(token,"short");
    click_label("Save & restart");
    require(find_prefix(lv_scr_act(),"Access token needs"),"missing useful token validation");
    require(!std::strcmp(model.settings.sniffer.host,original.sniffer.host),"invalid token saved settings");
    token=named_control("Access token",&lv_textarea_class);
    lv_textarea_set_text(token,"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    click_label("Connection options");
    lv_textarea_set_text(named_control("Stale after (2-60 seconds)",&lv_textarea_class),"9.5");
    lv_textarea_set_text(named_control("Refresh every (0.2-5 seconds)",&lv_textarea_class),"0.25");
    click_label("Save & restart");
    const auto& saved=model.settings.sniffer;
    require(!std::strcmp(saved.host,"192.0.2.77") && saved.enabled && saved.apply_on_connect && saved.timeout_ms==9500 && saved.refresh_ms==250,"connection units/save mapping failed");
    require(saved.radio.mode==sniffer::Mode::Scan && saved.radio.profile_count==3,"mode/scan mapping failed");
    require(saved.radio.profiles[0].frequency_hz==869525000 && saved.radio.profiles[2].frequency_hz==867500001 &&
        saved.radio.profiles[2].sf==9 && saved.radio.profiles[2].cr==7 && saved.radio.profiles[2].sync_word==0x2B,"radio tuning/preset save mapping failed");
    require(!std::strcmp(model.settings.ssid,original.ssid) && !std::strcmp(model.settings.hosts[0].address,original.hosts[0].address),"LoRa save changed unrelated settings");
    click_label(LV_SYMBOL_CLOSE); model.settings=original;
    if (model.simulated) {
        click_label(LV_SYMBOL_SETTINGS); click_label("Receiver"); choose("Tuning preset",2); click_label("Apply radio");
        require(model.lora.active.frequency_hz==869525000 &&
            model.settings.sniffer.radio.profiles[0].frequency_hz==original.sniffer.radio.profiles[0].frequency_hz,"live apply persisted the radio draft");
        require(find_prefix(lv_scr_act(),"Queued, not saved"),"live apply feedback implies persistence");
        click_label(LV_SYMBOL_CLOSE); model.lora=original_state;
    }
    click_label(LV_SYMBOL_SETTINGS); click_label("Receiver");
    model.lora.connected=false; lv_tick_inc(200); lv_timer_handler();
    require(lv_obj_has_state(lv_obj_get_parent(find_text(lv_scr_act(),"Apply radio")),LV_STATE_DISABLED),"offline live apply is enabled");
    choose("Tuning preset",2); click_label(LV_SYMBOL_CLOSE);
    require(model.settings.sniffer.radio.profiles[0].frequency_hz==original.sniffer.radio.profiles[0].frequency_hz,"cancel persisted LoRa edits");
    model.lora=original_state; dash::ui::navigate(0);
}
static void host_overlay_tests() {
    dash::ui::navigate(1); lv_obj_update_layout(lv_scr_act());
    for (unsigned i=0;i<4;++i) {
        const auto original=model.settings.hosts[i];
        const int x=i==0?34:i==1?414:120; // decorative LED/chart must not swallow row touches
        touch_at(x,252+int(i)*45,true,700); touch_at(x,252+int(i)*45,false,80);
        char title[32]; std::snprintf(title,sizeof(title),"Host %u settings",i+1);
        require(find_text(lv_scr_act(),title),"long press did not open selected host editor");
        require(!find_text(lv_scr_act(),"Wi-Fi SSID"),"host long press opened general settings");
        auto* name=find_input_text(lv_scr_act(),original.name);
        require(name && find_input_text(lv_scr_act(),original.address),"editor has wrong host values");
        check_modal_blocks_tabs(title);
        lv_textarea_set_text(name,"Unsaved change");
        click_label(LV_SYMBOL_CLOSE);
        require(!std::strcmp(model.settings.hosts[i].name,original.name),"cancel changed saved host");
    }
    touch_at(120,297,true,80); touch_at(120,297,false,80);
    if (model.settings.hosts[1].address[0]) click_label("Edit");
    require(find_text(lv_scr_act(),"Host 2 settings"),"short-click detail Edit or empty-host shortcut failed");
    click_label(LV_SYMBOL_CLOSE);
    const auto original=model.settings;
    touch_at(120,342,true,700); touch_at(120,342,false,80);
    auto* name=find_input_text(lv_scr_act(),model.settings.hosts[2].name);
    auto* address=find_input_text(lv_scr_act(),model.settings.hosts[2].address);
    require(name && address,"host 3 editor missing inputs");
    lv_textarea_set_text(name,"Edited host 3"); lv_textarea_set_text(address,"192.0.2.99");
    click_label("Save");
    require(!std::strcmp(model.settings.hosts[2].name,"Edited host 3") &&
        !std::strcmp(model.settings.hosts[2].address,"192.0.2.99"),"host save did not update selected slot");
    for (unsigned i : {0u,1u,3u})
        require(!std::strcmp(model.settings.hosts[i].name,original.hosts[i].name) &&
            !std::strcmp(model.settings.hosts[i].address,original.hosts[i].address),"host save changed another slot");
    require(!std::strcmp(model.settings.ssid,original.ssid) && !std::strcmp(model.settings.sniffer.token,original.sniffer.token),"host save changed unrelated settings");
    click_label(LV_SYMBOL_CLOSE);
    click_label(LV_SYMBOL_SETTINGS); check_modal_blocks_tabs("Settings"); click_label(LV_SYMBOL_CLOSE);
    dash::ui::navigate(0);
}
static void self_test() {
    const auto original_settings=model.settings;
    model.settings.screen_timeout_s=1; // old saved timeouts must have no effect
    model.settings.rotation_s=0;
    dash::ui::navigate(1);
    touch_at(200,40,false,3000);
    require(backlight!=0,"idle timeout still blanked the screen");
    dash::ui::toggle_screen();
    require(backlight==0,"physical-button request did not blank the screen");
    touch_at(64,452,true,100); touch_at(64,452,false,100);
    require(backlight==0 && find_text(lv_scr_act(),"HOST HEALTH"),"dark-screen touch woke or navigated the display");
    touch_at(64,452,true,100); // hold a finger over Climate while waking
    dash::ui::toggle_screen();
    require(backlight==model.settings.brightness,"button did not restore saved brightness");
    touch_at(64,452,true,100); touch_at(64,452,false,100);
    require(find_text(lv_scr_act(),"HOST HEALTH"),"wake leaked a held touch into navigation");
    touch_at(64,452,true,100); touch_at(64,452,false,100);
    require(find_text(lv_scr_act(),"INDOOR"),"fresh touch failed after button wake");
    touch_at(200,40,false,3000);
    require(backlight!=0,"screen reblanked after manual wake");
    model.settings=original_settings;
    host_overlay_tests();
    lora_settings_tests();
    // Exercise the actual immediate-press navigation event, not just its API.
    for (const char* title : {"Network", "Climate"}) {
        auto* label = find_text(lv_scr_act(), title);
        // Climate heading exists too: select its bottom-bar label by y position.
        if (!std::strcmp(title, "Climate")) {
            for (uint32_t i = 0; i < lv_obj_get_child_cnt(lv_scr_act()); ++i) {
                auto* candidate = lv_obj_get_child(lv_scr_act(), i);
                if (lv_obj_get_y(candidate) >= 420 && find_text(candidate, title)) { label = find_text(candidate, title); break; }
            }
        }
        if (!label) std::exit(1);
        lv_event_send(lv_obj_get_parent(label), LV_EVENT_PRESSED, nullptr);
    }
    for (unsigned iteration = 0; iteration < 40; ++iteration) {
        for (unsigned p = 0; p < 4; ++p) { dash::ui::navigate(p); dash::ui::refresh(clock_ms); lv_tick_inc(200); lv_timer_handler(); }
        dash::ui::navigate(0);
        click_label("INDOOR"); click_label(LV_SYMBOL_CLOSE);
        click_label(LV_SYMBOL_SETTINGS);
        auto* input = find_type(lv_scr_act(), &lv_textarea_class);
        if (!input) std::exit(1);
        lv_event_send(input, LV_EVENT_FOCUSED, nullptr);
        auto* keyboard = find_type(lv_scr_act(), &lv_keyboard_class);
        if (!keyboard) std::exit(1);
        lv_event_send(keyboard, LV_EVENT_READY, nullptr);
        click_label(LV_SYMBOL_CLOSE);
        dash::ui::navigate(3);
        for (const char* range : {"5m","15m","60s"}) click_label(range);
        if (model.simulated) {
            auto* row=find_prefix(lv_scr_act(),"24:00:00");
            if (!row) { std::fprintf(stderr,"UI test missing recent packet, iteration %u / count %zu\n",iteration,model.lora.count()); std::exit(1); }
            lv_event_send(lv_obj_get_parent(row),LV_EVENT_CLICKED,nullptr);
            if (!find_prefix(lv_scr_act(),"Sniffer uptime")) { std::fprintf(stderr,"UI test packet detail did not open, iteration %u\n",iteration); std::exit(1); }
            click_label(LV_SYMBOL_CLOSE);
        }
        click_label("RECEIVED"); click_label(LV_SYMBOL_CLOSE);
        click_label(LV_SYMBOL_SETTINGS);
        for (const char* tab : {"Receiver","Connection"}) click_label(tab);
        auto* lora_input=find_type(lv_scr_act(),&lv_textarea_class);
        lv_event_send(lora_input,LV_EVENT_FOCUSED,nullptr);
        auto* lora_keyboard=find_type(lv_scr_act(),&lv_keyboard_class);
        if (!lora_keyboard) std::exit(1);
        lv_event_send(lora_keyboard,LV_EVENT_CANCEL,nullptr);
        click_label(LV_SYMBOL_CLOSE);
    }
    clock_ms += 300000; dash::ui::refresh(clock_ms);
    std::puts("UI: host editor/modal isolation; friendly LoRa setup, presets, units, scan editing, validation, offline apply, save/cancel; navigation, details, keyboard and stale states passed");
}
int main(int argc, char** argv) {
    bool simulated = false, unavailable = false, test = false;
    std::string screenshot,settings_view;
    unsigned page = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--simulate")) simulated = true;
        else if (!std::strcmp(argv[i], "--self-test")) test = true;
        else if (!std::strcmp(argv[i], "--unavailable")) unavailable = true;
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--page") && i + 1 < argc) page = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i],"--settings") && i+1<argc) settings_view=argv[++i];
        else { std::fprintf(stderr, "Usage: simulator [--simulate|--unavailable] [--page 0..3] [--settings connection|receiver|scan|tuning] [--screenshot image.ppm] [--self-test]\n"); return 2; }
    }
    if (page > 3 || (simulated && unavailable)) return 2;
    if (!settings_view.empty()) {
        if (settings_view!="connection" && settings_view!="receiver" && settings_view!="scan" && settings_view!="tuning") return 2;
        page=3;
    }
    if (simulated) simulate();
    lv_init();
    lv_disp_draw_buf_t buffer; lv_disp_draw_buf_init(&buffer, draw_buffer.data(), nullptr, draw_buffer.size());
    lv_disp_drv_t display; lv_disp_drv_init(&display); display.hor_res = 480; display.ver_res = 480;
    display.draw_buf = &buffer; display.flush_cb = flush; lv_disp_drv_register(&display);
    lv_indev_drv_t pointer; lv_indev_drv_init(&pointer); pointer.type = LV_INDEV_TYPE_POINTER; pointer.read_cb = input;
    lv_indev_drv_register(&pointer);
    dash::ui::create(model); dash::ui::navigate(page);
    if (test) { self_test(); return 0; }
    if (!settings_view.empty()) {
        click_label(LV_SYMBOL_SETTINGS);
        if (settings_view!="connection") click_label("Receiver");
        if (settings_view=="scan") { choose("Listen mode",1); choose("Scan size",1); }
        if (settings_view=="tuning") click_label("Edit tuning details");
    }
    if (!screenshot.empty()) {
        for (unsigned frame = 0; frame < 35; ++frame) { lv_tick_inc(20); lv_timer_handler(); }
        dash::ui::refresh(clock_ms); lv_refr_now(nullptr); write_ppm(screenshot); return 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    auto* window = SDL_CreateWindow("SenseCAP / explicit desktop simulation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 480, 0);
    if (!window) return 1;
    auto* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    auto* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 480, 480);
    if (!renderer || !texture) return 1;
    bool running = true; uint32_t previous = SDL_GetTicks(), last_refresh = previous;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_MOUSEMOTION) { pointer_x = event.motion.x; pointer_y = event.motion.y; }
            if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                pointer_x = event.button.x; pointer_y = event.button.y; pressed = event.type == SDL_MOUSEBUTTONDOWN;
            }
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_RIGHT) dash::ui::navigate(page = (page + 1) % 4);
                if (event.key.keysym.sym == SDLK_LEFT) dash::ui::navigate(page = (page + 3) % 4);
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
            }
        }
        const uint32_t now = SDL_GetTicks(); clock_ms += now - previous; lv_tick_inc(now - previous); previous = now;
        if (now - last_refresh >= model.settings.refresh_ms) {
            if (simulated) {
                for (size_t m = 0; m < 4; ++m) {
                    model.readings[m].at_ms = clock_ms;
                    model.history.add({dash::Metric(m), model.readings[m].value, clock_ms});
                }
                model.network.uptime_ms = clock_ms;
                static uint64_t last_lora=clock_ms, sequence=120;
                if (clock_ms-last_lora>=7000) { simulate_lora(clock_ms,++sequence); last_lora=clock_ms; }
                model.lora.last_message_ms=clock_ms;
                for (auto& h : model.network.hosts) if (h.online) h.last_ok_ms = clock_ms;
            }
            dash::ui::refresh(clock_ms); last_refresh = now;
        }
        lv_timer_handler(); SDL_UpdateTexture(texture, nullptr, pixels.data(), 480 * sizeof(uint32_t));
        SDL_RenderCopy(renderer, texture, nullptr, nullptr); SDL_RenderPresent(renderer); SDL_Delay(5);
    }
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
}
