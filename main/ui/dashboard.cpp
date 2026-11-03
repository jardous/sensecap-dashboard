#include "dashboard.hpp"
#include "app/platform.hpp"
#include "lvgl.h"
#include "widgets.hpp"
#include "lora_page.hpp"
#include "lora_settings_model.hpp"
#include "lora/client.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

namespace dash::ui {
namespace {
constexpr uint32_t BG = 0x0B1018, CARD = 0x17212D, EDGE = 0x263746;
constexpr uint32_t CYAN = 0x45D9E8, GREEN = 0x49DEAC, AMBER = 0xFFC16B;
constexpr uint32_t RED = 0xFF7D84, WHITE = 0xEDF5FA, MUTED = 0x96ABBE;
ViewModel* vm;
lv_obj_t *screen, *content, *heading, *subtitle, *status, *nav[4];
lv_obj_t *temperature, *humidity, *co2, *voc, *dew, *absolute, *comfort;
lv_obj_t *gauge, *humid_bar, *trend, *history_chart, *co2_chart, *voc_chart;
lv_chart_series_t *history_series, *co2_series, *voc_series;
lv_obj_t *network_info, *host_labels[4], *host_leds[4], *host_charts[4];
lv_chart_series_t* host_series[4];
lv_obj_t *modal = nullptr, *modal_backdrop = nullptr, *keyboard = nullptr, *settings_error = nullptr;
lv_obj_t* settings_form = nullptr;
lv_coord_t settings_form_height = 316;
Settings draft;
unsigned current_page = 0;
uint32_t window_minutes = 60;
uint64_t last_rotation = 0;
bool sleeping = false;
bool lora_settings_open = false;
void lora_update_actions();
void lora_update_summary();
bool lora_collect();
const char* page_names[] = {"Climate", "Network", "Reticulum", "LoRa"};
const char* page_icons[] = {LV_SYMBOL_HOME, LV_SYMBOL_WIFI, LV_SYMBOL_SHUFFLE, LV_SYMBOL_GPS};
lv_color_t color(uint32_t c) { return lv_color_hex(c); }

lv_obj_t* panel(lv_obj_t* parent, int x, int y, int w, int h, uint32_t background = CARD) {
    auto* o = lv_obj_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color(background), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 18, 0); lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, color(EDGE), 0);
    if (background == CARD) {
        lv_obj_set_style_bg_grad_color(o, color(0x121B26), 0);
        lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    }
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}
lv_obj_t* text(lv_obj_t* parent, const char* s, int x, int y,
               const lv_font_t* font = &lv_font_montserrat_16, uint32_t c = WHITE) {
    auto* o = lv_label_create(parent); lv_label_set_text(o, s); lv_obj_set_pos(o, x, y);
    lv_obj_set_style_text_font(o, font, 0); lv_obj_set_style_text_color(o, color(c), 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}
void set_text(lv_obj_t* o, const char* s) {
    if (std::strcmp(lv_label_get_text(o), s)) lv_label_set_text(o, s);
}
lv_obj_t* button(lv_obj_t* parent, const char* s, int x, int y, int w, int h,
                 lv_event_cb_t cb, void* user = nullptr) {
    auto* o = lv_btn_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 10, 0); lv_obj_set_style_bg_color(o, color(EDGE), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, color(0x294C59), LV_STATE_PRESSED);
    auto* label = text(o, s, 0, 0); lv_obj_center(label);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
    return o;
}
void metric_text(lv_obj_t* label, const Reading& reading, uint64_t now, const char* format) {
    char buffer[48];
    if (reading.fresh(now, vm->settings.stale_ms)) std::snprintf(buffer, sizeof(buffer), format, double(reading.value));
    else std::strcpy(buffer, "--");
    set_text(label, buffer);
    lv_obj_set_style_text_color(label, color(reading.fresh(now, vm->settings.stale_ms) ? CYAN : MUTED), 0);
}
lv_obj_t* chart(lv_obj_t* parent, int x, int y, int w, int h,
                lv_chart_series_t** series, uint32_t c = CYAN, unsigned count = 60) {
    auto* o = lv_chart_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_chart_set_type(o, LV_CHART_TYPE_LINE); lv_chart_set_point_count(o, count);
    lv_chart_set_div_line_count(o, 0, 0);
    lv_obj_set_style_line_width(o, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(o, 0, LV_PART_INDICATOR);
    *series = lv_chart_add_series(o, color(c), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(o, *series, LV_CHART_POINT_NONE);
    return o;
}
void chart_values(lv_obj_t* chart_obj, lv_chart_series_t* series, const float* values,
                   size_t n, float minimum_span) {
    float lo = INFINITY, hi = -INFINITY;
    for (size_t i = 0; i < n; ++i) if (std::isfinite(values[i])) {
        lo = std::min(lo, values[i]); hi = std::max(hi, values[i]);
    }
    if (!std::isfinite(lo)) { lo = 0; hi = minimum_span; }
    if (hi - lo < minimum_span) { const float center = (hi + lo) / 2; lo = center - minimum_span / 2; hi = center + minimum_span / 2; }
    lv_chart_set_range(chart_obj, LV_CHART_AXIS_PRIMARY_Y, int(std::floor(lo * 10)) - 1, int(std::ceil(hi * 10)) + 1);
    for (size_t i = 0; i < n; ++i) series->y_points[i] = std::isfinite(values[i]) ? lv_coord_t(values[i] * 10) : LV_CHART_POINT_NONE;
    lv_chart_refresh(chart_obj);
}
void wait_for_release() {
    if (auto* input=lv_indev_get_act()) lv_indev_wait_release(input);
}
void open_modal(int x,int y,int w,int h) {
    assert(!modal && !modal_backdrop);
    // Transparent hit-test shield: no dimming/fade or expensive full-screen
    // compositing, but background controls cannot acquire a pressed state.
    modal_backdrop=lv_obj_create(screen); lv_obj_remove_style_all(modal_backdrop);
    lv_obj_set_pos(modal_backdrop,0,0); lv_obj_set_size(modal_backdrop,480,480);
    lv_obj_clear_flag(modal_backdrop,LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(modal_backdrop,LV_OBJ_FLAG_CLICKABLE);
    modal=panel(modal_backdrop,x,y,w,h);
    wait_for_release(); // the opening long-press must not click through on release
}
void close_modal(lv_event_t*) {
    wait_for_release();
    if (keyboard) { lv_obj_del(keyboard); keyboard = nullptr; }
    if (modal_backdrop) lv_obj_del(modal_backdrop);
    modal=modal_backdrop=nullptr;
    settings_form=settings_error=nullptr;
    lora_settings_open=false;
}
void show_settings(lv_event_t*);
void open_host_settings(size_t index);
void host_settings(lv_event_t* e) {
    if (!modal) open_host_settings(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
}
void details(lv_event_t* e) {
    if (modal) return;
    const auto metric = Metric(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    open_modal(12,60,456,356);
    lv_obj_set_style_shadow_width(modal, 30, 0); lv_obj_set_style_shadow_opa(modal, 150, 0);
    const char* names[] = {"Indoor temperature", "Relative humidity", "Carbon dioxide", "VOC index"};
    text(modal, names[size_t(metric)], 20, 20, &lv_font_montserrat_24);
    button(modal, LV_SYMBOL_CLOSE, 390, 12, 48, 44, close_modal);
    const auto& r = vm->readings[size_t(metric)];
    char value[64];
    if (r.fresh(platform::now_ms(), vm->settings.stale_ms))
        std::snprintf(value, sizeof(value), "%.1f %s", double(r.value), size_t(metric) == 0 ? "°C" : size_t(metric) == 1 ? "%" : size_t(metric) == 2 ? "ppm" : "index");
    else std::strcpy(value, "Unavailable");
    text(modal, value, 20, 65, &lv_font_montserrat_40, CYAN);
    text(modal, "Historical measurements / selected window", 20, 124, &lv_font_montserrat_16, MUTED);
    lv_chart_series_t* series;
    auto* c = chart(modal, 22, 162, 408, 104, &series);
    float values[60]; vm->history.chart(metric, platform::now_ms(), window_minutes, values, 60);
    chart_values(c, series, values, 60, size_t(metric) < 2 ? 2 : 100);
    auto* note = text(modal, size_t(metric) == 3 ? "SGP40 relative VOC index, not a gas concentration."
                     : "Gaps indicate missing data. History resets on reboot.", 20, 292, &lv_font_montserrat_16, MUTED);
    lv_obj_set_width(note, 412); lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}
void clickable_metric(lv_obj_t* o, Metric metric) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, details, LV_EVENT_SHORT_CLICKED, reinterpret_cast<void*>(size_t(metric)));
    lv_obj_add_event_cb(o, show_settings, LV_EVENT_LONG_PRESSED, nullptr);
}
void range_clicked(lv_event_t* e) {
    window_minutes = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    refresh(platform::now_ms());
}
void make_climate() {
    auto* temp_card = panel(content, 18, 78, 204, 176);
    clickable_metric(temp_card, Metric::Temperature);
    gauge = lv_arc_create(temp_card); lv_obj_set_pos(gauge, 19, 3); lv_obj_set_size(gauge, 166, 166);
    lv_arc_set_bg_angles(gauge, 135, 405); lv_arc_set_range(gauge, -100, 450);
    lv_arc_set_value(gauge, -100);
    lv_obj_set_style_arc_color(gauge, color(EDGE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(gauge, color(CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(gauge, 7, LV_PART_MAIN); lv_obj_set_style_arc_width(gauge, 7, LV_PART_INDICATOR);
    lv_obj_remove_style(gauge, nullptr, LV_PART_KNOB); lv_obj_clear_flag(gauge, LV_OBJ_FLAG_CLICKABLE);
    auto* caption = text(temp_card, "INDOOR", 0, 35, &lv_font_montserrat_14, MUTED);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 35);
    temperature = text(temp_card, "--", 0, 59, &lv_font_montserrat_48, CYAN);
    lv_obj_align(temperature, LV_ALIGN_TOP_MID, 0, 59);
    caption = text(temp_card, "°C", 0, 113, &lv_font_montserrat_18, MUTED);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 113);
    comfort = text(temp_card, "Awaiting sensor", 0, 150, &lv_font_montserrat_14, MUTED);
    lv_obj_align(comfort, LV_ALIGN_TOP_MID, 0, 150);

    auto* humid = panel(content, 234, 78, 228, 176);
    clickable_metric(humid, Metric::Humidity);
    text(humid, "HUMIDITY", 16, 14, &lv_font_montserrat_14, MUTED);
    humidity = text(humid, "--", 16, 35, &lv_font_montserrat_40, CYAN);
    text(humid, "% RH", 151, 53, &lv_font_montserrat_16, MUTED);
    humid_bar = lv_bar_create(humid); lv_obj_set_pos(humid_bar, 16, 88); lv_obj_set_size(humid_bar, 196, 5);
    lv_obj_set_style_bg_color(humid_bar, color(EDGE), 0);
    lv_obj_set_style_bg_color(humid_bar, color(CYAN), LV_PART_INDICATOR);
    text(humid, "Dew point", 16, 108, &lv_font_montserrat_16, MUTED);
    dew = text(humid, "--", 136, 108, &lv_font_montserrat_16);
    text(humid, "Absolute", 16, 140, &lv_font_montserrat_16, MUTED);
    absolute = text(humid, "--", 116, 140, &lv_font_montserrat_16);

    auto* c = panel(content, 18, 266, 216, 76); clickable_metric(c, Metric::Co2);
    text(c, "CO2", 14, 10, &lv_font_montserrat_14, MUTED);
    co2 = text(c, "--", 14, 31, &lv_font_montserrat_28, CYAN);
    text(c, "ppm", 105, 45, &lv_font_montserrat_14, MUTED);
    co2_chart = chart(c, 143, 38, 57, 24, &co2_series, GREEN, 24);
    c = panel(content, 246, 266, 216, 76); clickable_metric(c, Metric::Voc);
    text(c, "VOC INDEX", 14, 10, &lv_font_montserrat_14, MUTED);
    voc = text(c, "--", 14, 31, &lv_font_montserrat_28, CYAN);
    voc_chart = chart(c, 133, 38, 66, 24, &voc_series, CYAN, 24);
    trend = text(content, "TEMPERATURE TREND", 20, 353, &lv_font_montserrat_14, MUTED);
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t windows[] = {60, 360, 1440};
        button(content, i == 0 ? "1h" : i == 1 ? "6h" : "24h", 292 + i * 57, 347, 52, 32,
               range_clicked, reinterpret_cast<void*>(windows[i]));
    }
    history_chart = chart(content, 22, 386, 436, 27, &history_series);
}
void make_network() {
    auto* p = panel(content, 18, 78, 444, 118);
    network_info = text(p, "Wi-Fi not configured\nOpen settings to connect", 16, 14, &lv_font_montserrat_16);
    lv_obj_set_style_text_line_space(network_info, 5, 0);
    lv_obj_set_width(network_info, 414); lv_label_set_long_mode(network_info, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(p,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p,show_settings,LV_EVENT_SHORT_CLICKED,nullptr);
    lv_obj_add_event_cb(p,show_settings,LV_EVENT_LONG_PRESSED,nullptr);
    text(content, "HOST HEALTH", 20, 208, &lv_font_montserrat_14, MUTED);
    text(content, "TCP / HTTP", 349, 208, &lv_font_montserrat_14, MUTED);
    for (size_t i = 0; i < 4; ++i) {
        p = panel(content, 18, 233 + i * 45, 444, 40);
        lv_obj_set_style_radius(p, 10, 0);
        host_leds[i] = panel(p, 12, 14, 9, 9, MUTED);
        lv_obj_set_style_radius(host_leds[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(host_leds[i],LV_OBJ_FLAG_CLICKABLE);
        host_labels[i] = text(p, "Not configured", 32, 10, &lv_font_montserrat_16, MUTED);
        lv_obj_set_width(host_labels[i], 324); lv_label_set_long_mode(host_labels[i], LV_LABEL_LONG_DOT);
        host_charts[i] = chart(p, 362, 9, 64, 23, &host_series[i], CYAN, 32);
        lv_obj_clear_flag(host_charts[i],LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(p, [](lv_event_t* event) {
            if (modal) return;
            const size_t i = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
            const auto& cfg = vm->settings.hosts[i]; const auto& host = vm->network.hosts[i];
            if (!cfg.address[0]) { open_host_settings(i); return; }
            open_modal(12,60,456,356);
            auto* name = text(modal, cfg.address[0] ? cfg.name : "Host not configured", 20, 20, &lv_font_montserrat_24);
            lv_obj_set_width(name, 270); lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            button(modal,"Edit",302,12,78,44,[](lv_event_t* e) {
                const auto index=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
                close_modal(nullptr); open_host_settings(index);
            },reinterpret_cast<void*>(i));
            button(modal, LV_SYMBOL_CLOSE, 390, 12, 48, 44, close_modal);
            auto* address = text(modal, cfg.address[0] ? cfg.address : "Long-press a host to configure", 20, 66, &lv_font_montserrat_16, MUTED);
            lv_obj_set_width(address, 412); lv_label_set_long_mode(address, LV_LABEL_LONG_WRAP);
            char info[192];
            if (host.last_ok_ms) std::snprintf(info, sizeof(info), "%s check  /  last success %llus ago",
                cfg.check == Check::Tcp ? "TCP connect" : "HTTP HEAD", (unsigned long long)((platform::now_ms() - host.last_ok_ms) / 1000));
            else std::snprintf(info, sizeof(info), "%s check  /  no successful check yet", cfg.check == Check::Tcp ? "TCP connect" : "HTTP HEAD");
            text(modal, info, 20, 120, &lv_font_montserrat_16, MUTED);
            lv_chart_series_t* series;
            auto* c = chart(modal, 22, 164, 408, 100, &series, CYAN, 32);
            float points[32]; std::fill(points, points + 32, NAN);
            for (unsigned k = 0; k < host.count; ++k) points[32 - host.count + k] = host.history[(host.cursor + 32 - host.count + k) % 32];
            chart_values(c, series, points, 32, 20);
            auto* note = text(modal, "Latency includes DNS and the selected health check.\nGaps mean failed checks, not zero latency.", 20, 288, &lv_font_montserrat_16, MUTED);
            lv_obj_set_width(note, 412); lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        }, LV_EVENT_SHORT_CLICKED, reinterpret_cast<void*>(i));
        lv_obj_add_event_cb(p,host_settings,LV_EVENT_LONG_PRESSED,reinterpret_cast<void*>(i));
    }
}
void optional_page(bool reticulum) {
    auto* p = panel(content, 18, 78, 444, 336);
    auto* icon = text(p, reticulum ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_GPS, 0, 38, &lv_font_montserrat_40, CYAN);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 38);
    text(p, "EXTERNAL MODULE", 24, 110, &lv_font_montserrat_14, MUTED);
    text(p, "Not enabled", 24, 137, &lv_font_montserrat_28);
    auto* description = text(p, reticulum
        ? "Reticulum runs on your Linux host. A read-only, authenticated bridge is included. The device client is a later milestone."
        : "D1S has no built-in LoRa radio. An external Wi-Fi gateway and protocol-specific adapter are required. No radio traffic is simulated.",
        24, 184, &lv_font_montserrat_18, MUTED);
    lv_obj_set_width(description, 394); lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
}
void gesture(lv_event_t*) {
    if (modal) return;
    const auto direction = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (direction == LV_DIR_LEFT) navigate((current_page + 1) % 4);
    if (direction == LV_DIR_RIGHT) navigate((current_page + 3) % 4);
}
void nav_clicked(lv_event_t* e) { navigate(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))); }
void maintenance(lv_timer_t*) {
    const auto now = platform::now_ms();
    if (!modal && !sleeping && vm->settings.rotation_s && now - last_rotation >= vm->settings.rotation_s * 1000ull)
        navigate((current_page + 1) % 2);
    if (current_page==3 && !sleeping) refresh(now);
    if (lora_settings_open) lora_update_actions();
}
void arc_value(void* obj, int32_t value) { lv_arc_set_value(static_cast<lv_obj_t*>(obj), value); }
void animate_gauge(int value) {
    if (lv_arc_get_value(gauge) == value) return;
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, gauge); lv_anim_set_exec_cb(&a, arc_value);
    lv_anim_set_values(&a, lv_arc_get_value(gauge), value); lv_anim_set_time(&a, 450);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
}

// Settings use pointers into a separate draft; no credential ever appears in logs.
enum class FieldType { String, U32, U16, U8, Float, Check, Mode, MHz, Seconds, Hex };
struct Field { void* address; size_t capacity; FieldType type; lv_obj_t* input; };
std::array<Field, 56> fields;
size_t field_count = 0;
void input_focus(lv_event_t* e) {
    if (!keyboard) {
        keyboard = lv_keyboard_create(modal_backdrop); lv_obj_set_size(keyboard, 480, 210);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(keyboard, [](lv_event_t* ev) {
            if (lv_event_get_code(ev) == LV_EVENT_READY || lv_event_get_code(ev) == LV_EVENT_CANCEL) {
                lv_obj_del(keyboard); keyboard = nullptr;
                if (settings_form) lv_obj_set_height(settings_form, settings_form_height);
                if (lora_settings_open && lora_collect()) lora_update_summary();
            }
        }, LV_EVENT_ALL, nullptr);
    }
    lv_keyboard_set_textarea(keyboard, lv_event_get_target(e));
    // Scroll form only (not the modal/header) above the software keyboard.
    auto* form = lv_obj_get_parent(lv_event_get_target(e));
    lv_obj_update_layout(form); lv_area_t bounds; lv_obj_get_coords(form,&bounds);
    lv_obj_set_height(form,std::min(settings_form_height,lv_coord_t(std::max(70,270-int(bounds.y1)-8))));
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
}
void add_field(lv_obj_t* form, int& y, const char* title, void* address, FieldType type,
               size_t capacity = 0, bool secret = false) {
    text(form, title, 10, y, &lv_font_montserrat_16, MUTED);
    auto* input = lv_textarea_create(form); lv_obj_set_pos(input, 8, y + 24); lv_obj_set_size(input, 396, 43);
    lv_textarea_set_one_line(input, true); lv_textarea_set_password_mode(input, secret);
    if (secret) lv_textarea_set_password_show_time(input,0);
    lv_obj_set_style_bg_color(input, color(BG), 0); lv_obj_set_style_text_color(input, color(WHITE), 0);
    lv_obj_set_style_border_color(input, color(EDGE), 0);
    lv_obj_set_style_text_font(input, &lv_font_montserrat_18, 0);
    char buffer[40];
    if (type == FieldType::String) { lv_textarea_set_max_length(input, capacity - 1); lv_textarea_set_text(input, static_cast<char*>(address)); }
    else {
        if (type==FieldType::MHz) lora_settings_model::format_frequency(*static_cast<uint32_t*>(address),buffer,sizeof(buffer));
        else if (type==FieldType::Seconds) {
            std::snprintf(buffer,sizeof(buffer),"%.3f",*static_cast<uint32_t*>(address)/1000.0);
            lora_settings_model::compact_decimal(buffer);
        }
        else if (type==FieldType::Hex) std::snprintf(buffer,sizeof(buffer),"0x%02X",unsigned(*static_cast<uint8_t*>(address)));
        else if (type == FieldType::Float) std::snprintf(buffer, sizeof(buffer), "%.1f", double(*static_cast<float*>(address)));
        else std::snprintf(buffer, sizeof(buffer), "%lu", (unsigned long)(type == FieldType::U32 ? *static_cast<uint32_t*>(address) :
                     type == FieldType::U16 ? *static_cast<uint16_t*>(address) : type == FieldType::U8 ? *static_cast<uint8_t*>(address) :
                     type == FieldType::Mode ? unsigned(*static_cast<sniffer::Mode*>(address)) : unsigned(*static_cast<Check*>(address))));
        lv_textarea_set_text(input, buffer); lv_textarea_set_accepted_chars(input, "0123456789.-");
        lv_textarea_set_max_length(input, 10);
        if (type==FieldType::Hex) { lv_textarea_set_accepted_chars(input,"0123456789abcdefABCDEFxX"); lv_textarea_set_max_length(input,4); }
    }
    lv_obj_add_event_cb(input, input_focus, LV_EVENT_FOCUSED, nullptr);
    assert(field_count<fields.size());
    fields[field_count++] = {address, capacity, type, input}; y += 84;
}
bool collect_fields() {
    bool valid_input = true;
    for (size_t i = 0; i < field_count; ++i) {
        auto& field = fields[i]; const char* s = lv_textarea_get_text(field.input);
        lv_obj_set_style_border_color(field.input,color(EDGE),0);
        if (field.type == FieldType::String) std::snprintf(static_cast<char*>(field.address), field.capacity, "%s", s);
        else if (field.type==FieldType::MHz || field.type==FieldType::Seconds || field.type==FieldType::Hex) {
            const bool ok=field.type==FieldType::MHz?lora_settings_model::frequency_hz(s,*static_cast<uint32_t*>(field.address)):
                field.type==FieldType::Seconds?lora_settings_model::duration_ms(s,*static_cast<uint32_t*>(field.address)):
                lora_settings_model::sync_word(s,*static_cast<uint8_t*>(field.address));
            if (!ok) { lv_obj_set_style_border_color(field.input,color(RED),0); lv_obj_scroll_to_view(field.input,LV_ANIM_OFF); valid_input=false; break; }
        }
        else {
            char* end; const double value = std::strtod(s, &end);
            if (end == s || *end || !std::isfinite(value)) { valid_input = false; break; }
            if (field.type != FieldType::Float && (value < 0 || value != std::floor(value))) { valid_input = false; break; }
            if (field.type == FieldType::Float) *static_cast<float*>(field.address) = value;
            else if (field.type == FieldType::U32 && value <= UINT32_MAX) *static_cast<uint32_t*>(field.address) = uint32_t(value);
            else if (field.type == FieldType::U16 && value <= 65535) *static_cast<uint16_t*>(field.address) = uint16_t(value);
            else if (field.type == FieldType::U8 && value <= 255) *static_cast<uint8_t*>(field.address) = uint8_t(value);
            else if (field.type == FieldType::Check && value <= 1) *static_cast<Check*>(field.address) = Check(uint8_t(value));
            else if (field.type == FieldType::Mode && value <= 2) *static_cast<sniffer::Mode*>(field.address) = sniffer::Mode(uint8_t(value));
            else { valid_input = false; break; }
        }
    }
    return valid_input;
}
void save_clicked(lv_event_t*) {
    const bool valid_input = collect_fields();
    const bool ok = valid_input && validate_settings(draft) && platform::save_settings(draft);
    set_text(settings_error, ok ? "Queued. Reboots after successful save." : "Invalid value. Check ranges in README.");
    lv_obj_set_style_text_color(settings_error, color(ok ? GREEN : RED), 0);
}
void open_host_settings(size_t index) {
    if (modal || index>=vm->settings.hosts.size()) return;
    draft=vm->settings; field_count=0;
    open_modal(18,12,444,404);
    char title[32]; std::snprintf(title,sizeof(title),"Host %u settings",unsigned(index+1));
    text(modal,title,18,14,&lv_font_montserrat_24);
    button(modal,"Save",276,8,84,44,save_clicked);
    button(modal,LV_SYMBOL_CLOSE,374,8,52,44,close_modal);
    settings_form_height=316;
    settings_form=panel(modal,10,64,424,settings_form_height);
    lv_obj_add_flag(settings_form,LV_OBJ_FLAG_SCROLLABLE);
    auto& host=draft.hosts[index]; int y=4;
    add_field(settings_form,y,"Friendly name",host.name,FieldType::String,sizeof(host.name));
    add_field(settings_form,y,"Address (TCP hostname or full HTTP URL)",host.address,FieldType::String,sizeof(host.address));
    add_field(settings_form,y,"TCP port (HTTP uses URL port)",&host.port,FieldType::U16);
    add_field(settings_form,y,"Check: 0 = TCP, 1 = HTTP HEAD",&host.check,FieldType::Check);
    settings_error=text(settings_form,"Save updates this host only; reboots after commit.",10,y+8,&lv_font_montserrat_14,MUTED);
    lv_obj_set_width(settings_error,396); lv_label_set_long_mode(settings_error,LV_LABEL_LONG_WRAP);
}
unsigned lora_settings_tab=0, lora_profile_index=0;
bool lora_advanced[2]{};
lv_obj_t *lora_tabs[2],*lora_primary,*lora_summary=nullptr,*lora_preset=nullptr;
struct LoraChoice {
    void* address; bool wide;
    std::array<uint32_t,12> values{};
};
std::array<LoraChoice,4> lora_choices;
size_t lora_choice_count=0;
void lora_settings_fields();
void lora_message(const char* message,uint32_t tone=MUTED) {
    set_text(settings_error,message); lv_obj_set_style_text_color(settings_error,color(tone),0);
}
void lora_update_summary() {
    if (!lora_settings_open || !lora_summary) return;
    const auto& p=draft.sniffer.radio.profiles[lora_profile_index];
    char frequency[24],bandwidth[24],summary[160];
    lora_settings_model::format_frequency(p.frequency_hz,frequency,sizeof(frequency));
    std::snprintf(bandwidth,sizeof(bandwidth),"%.2f",p.bw_hz/1000.0); lora_settings_model::compact_decimal(bandwidth);
    std::snprintf(summary,sizeof(summary),"%s MHz / SF%u / BW %s kHz\nCR4/%u / sync 0x%02X / CRC %s",
        frequency,p.sf,bandwidth,p.cr,p.sync_word,p.crc_enabled?"on":"off");
    set_text(lora_summary,summary);
    lv_dropdown_set_selected(lora_preset,unsigned(lora_settings_model::identify(p)));
}
bool lora_collect() {
    if (collect_fields()) return true;
    lora_message("Check number format: MHz, seconds, or 0xNN sync.",RED); return false;
}
bool lora_profile_valid() {
    if (sniffer::valid(draft.sniffer.radio.profiles[lora_profile_index])) return true;
    lora_message("Name: letters/numbers/space - _ .; preamble: 4-65535.",RED); return false;
}
bool lora_link_changed() {
    const auto& a=draft.sniffer; const auto& b=vm->settings.sniffer;
    return a.enabled!=b.enabled || a.port!=b.port || std::strcmp(a.host,b.host) || std::strcmp(a.token,b.token);
}
void lora_update_actions() {
    if (!lora_settings_open) return;
    const bool disabled=lora_settings_tab==1 && (!vm->lora.connected || !vm->lora.enabled || lora_link_changed());
    if (disabled) lv_obj_add_state(lora_primary,LV_STATE_DISABLED);
    else lv_obj_clear_state(lora_primary,LV_STATE_DISABLED);
    lv_obj_set_style_text_color(lv_obj_get_child(lora_primary,0),color(disabled?MUTED:WHITE),0);
}
void lora_hint() {
    if (lora_settings_tab==0) {
        lora_message(vm->simulated?"Desktop preview only. No network connection is made.":
            vm->lora.connected?"Connected. Address/token changes need Save & restart.":
            !vm->network.connected?"First connect D1S Wi-Fi in Network settings.":
            "Enable, enter the XIAO address/token, then Save & restart.");
    } else if (lora_link_changed()) lora_message("Connection changed. Save & restart before live apply.",AMBER);
    else if (!vm->lora.connected) lora_message("Offline: save connection settings and restart first.",AMBER);
    else lora_message(draft.sniffer.radio.mode==sniffer::Mode::Cad?"CAD detects activity, not packets. Apply is not saved.":
        draft.sniffer.radio.mode==sniffer::Mode::Scan?"Scan has blind periods. Apply is live; Save persists.":
        "Fixed listens on one profile. Apply is live; Save persists.");
}
lv_obj_t* lora_dropdown(int& y,const char* title,const char* options,unsigned selected,
                       lv_event_cb_t callback,void* data=nullptr,int x=8,int width=396) {
    text(settings_form,title,x+2,y,&lv_font_montserrat_14,MUTED);
    auto* control=lv_dropdown_create(settings_form);
    lv_obj_set_pos(control,x,y+22); lv_obj_set_size(control,width,42);
    lv_obj_set_style_bg_color(control,color(BG),0); lv_obj_set_style_text_color(control,color(WHITE),0);
    lv_obj_set_style_border_color(control,color(EDGE),0);
    lv_obj_set_style_text_font(control,&lv_font_montserrat_16,0);
    lv_dropdown_set_options(control,options); lv_dropdown_set_selected(control,selected);
    if (auto* list=lv_dropdown_get_list(control)) lv_obj_set_style_max_height(list,176,0);
    if (callback) lv_obj_add_event_cb(control,callback,LV_EVENT_VALUE_CHANGED,data);
    y+=72; return control;
}
void lora_choice(int& y,const char* title,const char* options,void* address,bool wide,const uint32_t* values,size_t count) {
    assert(lora_choice_count<lora_choices.size() && count<=12);
    auto& binding=lora_choices[lora_choice_count++]; binding.address=address; binding.wide=wide;
    unsigned selected=0; const auto current=wide?*static_cast<uint32_t*>(address):*static_cast<uint8_t*>(address);
    for (size_t i=0;i<count;++i) { binding.values[i]=values[i]; if (values[i]==current) selected=i; }
    lora_dropdown(y,title,options,selected,[](lv_event_t* e) {
        auto& b=*static_cast<LoraChoice*>(lv_event_get_user_data(e));
        const auto value=b.values[lv_dropdown_get_selected(lv_event_get_target(e))];
        if (b.wide) *static_cast<uint32_t*>(b.address)=value; else *static_cast<uint8_t*>(b.address)=uint8_t(value);
        lora_update_summary();
    },&binding);
}
void lora_switch(int& y,const char* title,uint8_t* value) {
    auto* caption=text(settings_form,title,10,y+7,&lv_font_montserrat_16);
    lv_obj_set_width(caption,320); lv_label_set_long_mode(caption,LV_LABEL_LONG_WRAP);
    auto* control=lv_switch_create(settings_form); lv_obj_set_pos(control,350,y+2); lv_obj_set_size(control,52,30);
    if (*value) lv_obj_add_state(control,LV_STATE_CHECKED);
    lv_obj_add_event_cb(control,[](lv_event_t* e) {
        *static_cast<uint8_t*>(lv_event_get_user_data(e))=lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED)?1:0;
        lora_update_actions();
        lora_update_summary();
    },LV_EVENT_VALUE_CHANGED,value);
    y+=44;
}
void lora_tab_clicked(lv_event_t* e) {
    if (!lora_collect()) return;
    if (lora_settings_tab==1 && !lora_profile_valid()) return;
    lora_settings_tab=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    lora_settings_fields();
}
void lora_advanced_clicked(lv_event_t*) {
    if (!lora_collect()) return;
    lora_advanced[lora_settings_tab]=!lora_advanced[lora_settings_tab];
    lora_settings_fields();
}
void lora_mode_changed(lv_event_t* e) {
    auto* control=lv_event_get_target(e); const auto selected=lv_dropdown_get_selected(control);
    if (!lora_collect() || !lora_profile_valid()) { lv_dropdown_set_selected(control,unsigned(draft.sniffer.radio.mode)); return; }
    draft.sniffer.radio.mode=sniffer::Mode(selected); lora_profile_index=0;
    lora_settings_fields();
}
void lora_count_changed(lv_event_t* e) {
    auto* control=lv_event_get_target(e); const auto selected=lv_dropdown_get_selected(control);
    if (!lora_collect() || !lora_profile_valid()) { lv_dropdown_set_selected(control,draft.sniffer.radio.profile_count-1); return; }
    draft.sniffer.radio.profile_count=selected+1;
    lora_profile_index=std::min(lora_profile_index,unsigned(selected));
    lora_settings_fields();
}
void lora_profile_changed(lv_event_t* e) {
    auto* control=lv_event_get_target(e); const auto selected=lv_dropdown_get_selected(control);
    if (!lora_collect() || !lora_profile_valid()) { lv_dropdown_set_selected(control,lora_profile_index); return; }
    lora_profile_index=selected; lora_settings_fields();
}
void lora_preset_changed(lv_event_t* e) {
    auto* control=lv_event_get_target(e); const auto selected=lv_dropdown_get_selected(control);
    auto& profile=draft.sniffer.radio.profiles[lora_profile_index];
    if (!lora_collect()) { lv_dropdown_set_selected(control,unsigned(lora_settings_model::identify(profile))); return; }
    if (selected) profile=lora_settings_model::profile(lora_settings_model::Preset(selected));
    else lora_advanced[1]=true;
    lora_settings_fields();
}
void lora_primary_clicked(lv_event_t*) {
    if (lora_settings_tab==0) { close_modal(nullptr); return; }
    if (!lora_collect()) return;
    if (!sniffer::valid(draft.sniffer.radio)) { lora_message("Check profile names and scan dwell (0.25-60 seconds).",RED); return; }
    if (lora_link_changed()) { lora_message("Save & restart the changed connection before applying.",AMBER); return; }
    const bool ok=lora::apply_radio(draft.sniffer.radio);
    lora_message(ok?"Queued, not saved. Check the LoRa page for confirmation.":"XIAO is offline or busy. Radio was not applied.",ok?GREEN:RED);
}
void lora_save_clicked(lv_event_t*) {
    if (!lora_collect()) return;
    auto& s=draft.sniffer;
    if (!sniffer::safe_token(s.token,sizeof(s.token),s.enabled)) {
        lora_settings_tab=0; lora_settings_fields();
        lora_message("Access token needs 32-95 visible ASCII characters.",RED); return;
    }
    if (!sniffer::valid(s.radio)) {
        lora_settings_tab=1; lora_advanced[1]=true;
        for (unsigned i=0;i<s.radio.profile_count;++i) if (!sniffer::valid(s.radio.profiles[i])) { lora_profile_index=i; break; }
        lora_settings_fields(); lora_message("Check profile names and scan dwell (0.25-60 seconds).",RED); return;
    }
    if (!sniffer::valid(s)) {
        lora_settings_tab=0; lora_advanced[0]=true; lora_settings_fields();
        lora_message("Check address/port, timeout (2-60s), refresh (0.2-5s).",RED); return;
    }
    const bool ok=validate_settings(draft) && platform::save_settings(draft);
    lora_message(ok?"Save queued. D1S restarts only after successful commit.":"Could not queue save. No settings were written.",ok?GREEN:RED);
}
void lora_settings_fields() {
    if (keyboard) { lv_obj_del(keyboard); keyboard=nullptr; }
    lv_obj_set_height(settings_form,settings_form_height);
    lora_summary=lora_preset=nullptr;
    lv_obj_clean(settings_form); field_count=0; lora_choice_count=0; int y=4;
    auto* form=settings_form; auto& s=draft.sniffer;
    for (unsigned i=0;i<2;++i) {
        lv_obj_set_style_bg_color(lora_tabs[i],color(i==lora_settings_tab?0x18363F:EDGE),0);
        lv_obj_set_style_text_color(lv_obj_get_child(lora_tabs[i],0),color(i==lora_settings_tab?CYAN:WHITE),0);
    }
    set_text(lv_obj_get_child(lora_primary,0),lora_settings_tab==0?"Cancel":"Apply radio");
    if (lora_settings_tab==0) {
        lora_switch(y,"Connect to XIAO",&s.enabled);
        add_field(form,y,"XIAO address (hostname or IP)",s.host,FieldType::String,sizeof(s.host));
        add_field(form,y,"Access token",s.token,FieldType::String,sizeof(s.token),true);
        button(form,lora_advanced[0]?"Hide connection options":"Connection options",8,y,396,40,lora_advanced_clicked); y+=54;
        if (lora_advanced[0]) {
            add_field(form,y,"Port (default 8080)",&s.port,FieldType::U16);
            add_field(form,y,"Stale after (2-60 seconds)",&s.timeout_ms,FieldType::Seconds);
            add_field(form,y,"Refresh every (0.2-5 seconds)",&s.refresh_ms,FieldType::Seconds);
            add_field(form,y,"Recent packets to keep (1-32)",&s.history_size,FieldType::U16);
            auto* note=text(form,"Trusted LAN only: ws:// and local storage are not encrypted.",10,y,&lv_font_montserrat_14,MUTED);
            lv_obj_set_width(note,396); lv_label_set_long_mode(note,LV_LABEL_LONG_WRAP);
        }
    } else {
        lora_dropdown(y,"Listen mode","Fixed receive\nScan profiles\nActivity only (CAD)",unsigned(s.radio.mode),lora_mode_changed);
        if (s.radio.mode==sniffer::Mode::Scan) {
            int side=y;
            lora_dropdown(side,"Scan size","1 profile\n2 profiles\n3 profiles\n4 profiles",s.radio.profile_count-1,lora_count_changed,nullptr,8,188);
            char options[64]="";
            for (unsigned i=0;i<s.radio.profile_count;++i) {
                char option[16]; std::snprintf(option,sizeof(option),"%sProfile %u",i?"\n":"",i+1);
                std::strcat(options,option);
            }
            side=y; lora_dropdown(side,"Edit profile",options,lora_profile_index,lora_profile_changed,nullptr,216,188); y+=72;
            add_field(form,y,"Time per profile (0.25-60 seconds)",&s.radio.dwell_ms,FieldType::Seconds);
        }
        auto& p=s.radio.profiles[lora_profile_index];
        lora_preset=lora_dropdown(y,"Tuning preset","Custom settings\n867.5 MHz / SF8 (default)\nMeshtastic EU868 LongFast",unsigned(lora_settings_model::identify(p)),lora_preset_changed);
        auto* note=text(form,"",10,y,&lv_font_montserrat_14,CYAN); lora_summary=note;
        lv_obj_set_width(note,396); lv_label_set_long_mode(note,LV_LABEL_LONG_WRAP); y+=56;
        lora_switch(y,"Restore radio on reconnect",&s.apply_on_connect);
        button(form,lora_advanced[1]?"Hide tuning details":"Edit tuning details",8,y,396,40,lora_advanced_clicked); y+=54;
        if (lora_advanced[1]) {
            add_field(form,y,"Profile name",p.name,FieldType::String,sizeof(p.name));
            add_field(form,y,"Frequency (863-870 MHz)",&p.frequency_hz,FieldType::MHz);
            const uint32_t bandwidths[]={7810,10420,15630,20830,31250,41670,62500,125000,250000,500000};
            lora_choice(y,"Bandwidth","7.81 kHz\n10.42 kHz\n15.63 kHz\n20.83 kHz\n31.25 kHz\n41.67 kHz\n62.5 kHz\n125 kHz\n250 kHz\n500 kHz",&p.bw_hz,true,bandwidths,10);
            const uint32_t factors[]={5,6,7,8,9,10,11,12},rates[]={5,6,7,8};
            lora_choice(y,"Spreading factor","SF5\nSF6\nSF7\nSF8\nSF9\nSF10\nSF11\nSF12",&p.sf,false,factors,8);
            lora_choice(y,"Coding rate","4/5\n4/6\n4/7\n4/8",&p.cr,false,rates,4);
            add_field(form,y,"Sync word (hex, e.g. 0x12 or 0x2B)",&p.sync_word,FieldType::Hex);
            add_field(form,y,"Preamble symbols (4-65535)",&p.preamble,FieldType::U16);
            lora_switch(y,"Packet CRC",&p.crc_enabled);
            note=text(form,"Explicit headers only. Presets tune the receiver; they do not identify or decrypt a protocol.",10,y,&lv_font_montserrat_14,MUTED);
            lv_obj_set_width(note,396); lv_label_set_long_mode(note,LV_LABEL_LONG_WRAP);
        }
    }
    lv_obj_update_layout(form); lv_obj_scroll_to_y(form,0,LV_ANIM_OFF);
    lora_update_summary(); lora_hint(); lora_update_actions();
}
void show_lora_settings() {
    draft=vm->settings; lora_settings_tab=0; lora_profile_index=0; lora_advanced[0]=lora_advanced[1]=false;
    open_modal(18,12,444,404); lora_settings_open=true;
    text(modal,"LoRa settings",18,16,&lv_font_montserrat_24);
    button(modal,LV_SYMBOL_CLOSE,374,8,52,44,close_modal);
    lora_tabs[0]=button(modal,"Connection",12,60,204,38,lora_tab_clicked,nullptr);
    lora_tabs[1]=button(modal,"Receiver",228,60,204,38,lora_tab_clicked,reinterpret_cast<void*>(uintptr_t(1)));
    settings_form_height=202;
    settings_form=panel(modal,10,104,424,settings_form_height,CARD);
    lv_obj_set_style_border_width(settings_form,0,0); lv_obj_add_flag(settings_form,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(settings_form,LV_SCROLLBAR_MODE_ON);
    lv_obj_set_scroll_dir(settings_form,LV_DIR_VER);
    lv_obj_set_style_width(settings_form,3,LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(settings_form,color(MUTED),LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(settings_form,LV_OPA_50,LV_PART_SCROLLBAR);
    settings_error=text(modal,"",16,310,&lv_font_montserrat_14,MUTED);
    lv_obj_set_width(settings_error,412); lv_label_set_long_mode(settings_error,LV_LABEL_LONG_WRAP);
    lora_primary=button(modal,"Cancel",12,352,166,44,lora_primary_clicked);
    button(modal,"Save & restart",190,352,242,44,lora_save_clicked);
    lora_settings_fields();
}
void show_settings(lv_event_t*) {
    if (modal) return;
    if (current_page==3) { show_lora_settings(); return; }
    draft = vm->settings; field_count = 0;
    open_modal(18,12,444,404);
    text(modal, "Settings", 18, 14, &lv_font_montserrat_24); // child 0
    button(modal, "Save", 276, 8, 84, 44, save_clicked); // child 1
    button(modal, LV_SYMBOL_CLOSE, 374, 8, 52, 44, close_modal); // child 2
    auto* form = panel(modal, 10, 64, 424, 316, CARD); // child 3
    settings_form=form; settings_form_height=316;
    lv_obj_set_style_border_width(form, 0, 0); lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE);
    int y = 4;
    add_field(form, y, "Wi-Fi SSID", draft.ssid, FieldType::String, sizeof(draft.ssid));
    add_field(form, y, "Wi-Fi password", draft.wifi_password, FieldType::String, sizeof(draft.wifi_password), true);
    add_field(form, y, "MQTT URI (empty = disabled)", draft.mqtt_uri, FieldType::String, sizeof(draft.mqtt_uri));
    add_field(form, y, "MQTT user", draft.mqtt_user, FieldType::String, sizeof(draft.mqtt_user));
    add_field(form, y, "MQTT password", draft.mqtt_password, FieldType::String, sizeof(draft.mqtt_password), true);
    add_field(form, y, "Device ID (letters, numbers, _ or -)", draft.device_id, FieldType::String, sizeof(draft.device_id));
    add_field(form, y, "Local NTP server (needed for TLS clock)", draft.ntp_server, FieldType::String, sizeof(draft.ntp_server));
    add_field(form, y, "Publish interval (5..3600 s)", &draft.publish_seconds, FieldType::U32);
    add_field(form, y, "UI refresh (250..60000 ms)", &draft.refresh_ms, FieldType::U32);
    add_field(form, y, "Stale timeout (15000..300000 ms)", &draft.stale_ms, FieldType::U32);
    add_field(form, y, "Brightness (5..100 %)", &draft.brightness, FieldType::U8);
    text(form,"Screen: physical button only / no idle blanking",10,y,&lv_font_montserrat_14,MUTED); y+=36;
    add_field(form, y, "Page rotation (0=off, min 10 s)", &draft.rotation_s, FieldType::U16);
    add_field(form, y, "CO2 warning (ppm)", &draft.limits.co2_warn, FieldType::Float);
    add_field(form, y, "CO2 critical (> warning)", &draft.limits.co2_critical, FieldType::Float);
    add_field(form, y, "VOC warning (index)", &draft.limits.voc_warn, FieldType::Float);
    add_field(form, y, "VOC critical (> warning)", &draft.limits.voc_critical, FieldType::Float);
    add_field(form, y, "Comfort temperature low", &draft.limits.temp_low, FieldType::Float);
    add_field(form, y, "Comfort temperature high", &draft.limits.temp_high, FieldType::Float);
    add_field(form, y, "Comfort humidity low", &draft.limits.rh_low, FieldType::Float);
    add_field(form, y, "Comfort humidity high", &draft.limits.rh_high, FieldType::Float);
    for (size_t i = 0; i < draft.hosts.size(); ++i) {
        char label[64]; auto& h = draft.hosts[i];
        std::snprintf(label, sizeof(label), "Host %u / friendly name", unsigned(i + 1));
        add_field(form, y, label, h.name, FieldType::String, sizeof(h.name));
        add_field(form, y, "Address (TCP hostname or full HTTP URL)", h.address, FieldType::String, sizeof(h.address));
        add_field(form, y, "TCP port (HTTP uses URL port)", &h.port, FieldType::U16);
        add_field(form, y, "Check: 0 = TCP, 1 = HTTP HEAD", &h.check, FieldType::Check);
    }
    text(form, "LoRa: use settings on its own page.", 10, y, &lv_font_montserrat_14, MUTED); y += 36;
    add_field(form, y, "Reserved Reticulum bridge URL", draft.reticulum_url, FieldType::String, sizeof(draft.reticulum_url));
    add_field(form, y, "Reserved Reticulum token", draft.reticulum_token, FieldType::String, sizeof(draft.reticulum_token), true);
    add_field(form, y, "Reserved LoRa gateway MQTT topic", draft.lora_topic, FieldType::String, sizeof(draft.lora_topic));
    settings_error = text(form, "Local storage is not encrypted. Use trusted LAN.", 10, y + 8, &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(settings_error, 396);
}
} // namespace

void toggle_screen() {
    sleeping=!sleeping;
    // A dark-screen touch must neither activate controls nor wake the panel.
    // Waiting for release also cancels any touch held across the button press,
    // without turning the release into a click on a newly visible control.
    for (auto* input=lv_indev_get_next(nullptr);input;input=lv_indev_get_next(input)) {
        lv_indev_wait_release(input);
        lv_indev_enable(input,!sleeping);
    }
    if (!sleeping) {
        last_rotation=platform::now_ms();
        lv_disp_trig_activity(nullptr);
        lv_obj_invalidate(screen);
    }
    platform::apply_backlight(sleeping?0:vm->settings.brightness);
}
void navigate(unsigned page) {
    if (modal || page > 3) return;
    if (page == current_page && lv_obj_get_child_cnt(content)) return;
    current_page = page; last_rotation = platform::now_ms();
    lv_obj_clean(content);
    set_text(heading, page_names[page]);
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_set_style_bg_color(nav[i], color(i == page ? 0x18363F : BG), 0);
        lv_obj_set_style_text_color(nav[i], color(i == page ? CYAN : MUTED), 0);
        // Text children carry their own style.
        for (uint32_t j = 0; j < lv_obj_get_child_cnt(nav[i]); ++j)
            lv_obj_set_style_text_color(lv_obj_get_child(nav[i], j), color(i == page ? CYAN : MUTED), 0);
    }
    if (page == 0) make_climate(); else if (page == 1) make_network(); else if (page==2) optional_page(true);
    else { set_text(heading,"LoRa"); lora_page::create(content,*vm); }
    // No full-screen opacity fade: costly RGB565 compositing delays tap feedback.
    refresh(platform::now_ms());
}
void create(ViewModel& model) {
    vm = &model; screen = lv_scr_act();
    auto* theme = lv_theme_default_init(lv_disp_get_default(), color(CYAN), color(GREEN), true, &lv_font_montserrat_16);
    lv_disp_set_theme(lv_disp_get_default(), theme);
    lv_obj_set_style_bg_color(screen, color(BG), 0); lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_16, 0);
    heading = text(screen, "Climate", 20, 15, &lv_font_montserrat_28);
    status = text(screen, "", 254, 22, &lv_font_montserrat_14, MUTED);
    subtitle = text(screen, "Waiting for sensors", 20, 51, &lv_font_montserrat_14, MUTED);
    button(screen, LV_SYMBOL_SETTINGS, 414, 10, 48, 44, show_settings);
    content = panel(screen, 0, 0, 480, 422, BG);
    lv_obj_set_style_border_width(content, 0, 0); lv_obj_set_style_radius(content, 0, 0);
    lv_obj_move_background(content); lv_obj_add_event_cb(content, gesture, LV_EVENT_GESTURE, nullptr);
    for (unsigned i = 0; i < 4; ++i) {
        nav[i] = button(screen, "", 10 + i * 117, 426, 109, 50, nav_clicked, reinterpret_cast<void*>(uintptr_t(i)));
        lv_obj_remove_event_cb(nav[i], nav_clicked);
        lv_obj_add_event_cb(nav[i], nav_clicked, LV_EVENT_PRESSED, reinterpret_cast<void*>(uintptr_t(i)));
        lv_obj_clean(nav[i]);
        auto* icon = text(nav[i], page_icons[i], 0, 5, &lv_font_montserrat_18);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 5);
        auto* label = text(nav[i], page_names[i], 0, 30, &lv_font_montserrat_14);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 29);
    }
    lv_obj_add_event_cb(screen, gesture, LV_EVENT_GESTURE, nullptr);
    lv_timer_create(maintenance, 200, nullptr);
    navigate(0);
}
void refresh(uint64_t now) {
    set_text(status, vm->simulated ? "SIMULATED" : vm->network.connected ? LV_SYMBOL_WIFI "  LOCAL" : "OFFLINE");
    lv_obj_set_style_text_color(status, color(vm->simulated ? AMBER : vm->network.connected ? GREEN : MUTED), 0);
    char buffer[256];
    if (current_page == 0) {
        uint64_t newest = 0; bool any = false;
        for (const auto& r : vm->readings) if (std::isfinite(r.value)) { any = true; newest = std::max(newest, r.at_ms); }
        const Health health = air_quality(vm->readings, now, vm->settings.stale_ms, vm->settings.limits);
        const char* air = health == Health::Healthy ? "Air quality good" : health == Health::Warning ? "Air quality warning" :
                          health == Health::Critical ? "Ventilate room" : "Air quality unavailable";
        if (any) std::snprintf(buffer, sizeof(buffer), "%s  /  updated %llus ago", air, (unsigned long long)((now - newest) / 1000));
        else std::snprintf(buffer, sizeof(buffer), "%s  /  waiting", air);
        set_text(subtitle, buffer);
        lv_obj_set_style_text_color(subtitle, color(health == Health::Healthy ? GREEN : health == Health::Critical ? RED : health == Health::Warning ? AMBER : MUTED), 0);
        metric_text(temperature, vm->readings[0], now, "%.1f");
        lv_obj_align(temperature, LV_ALIGN_TOP_MID, 0, 59);
        metric_text(humidity, vm->readings[1], now, "%.1f");
        metric_text(co2, vm->readings[2], now, "%.0f"); metric_text(voc, vm->readings[3], now, "%.0f");
        lv_obj_set_style_text_font(co2, vm->readings[2].value >= 10000 ? &lv_font_montserrat_24 : &lv_font_montserrat_28, 0);
        const auto& l = vm->settings.limits;
        for (unsigned i = 2; i < 4; ++i) {
            const auto& r = vm->readings[i];
            const float warning = i == 2 ? l.co2_warn : l.voc_warn, critical = i == 2 ? l.co2_critical : l.voc_critical;
            lv_obj_set_style_text_color(i == 2 ? co2 : voc,
                color(!r.fresh(now, vm->settings.stale_ms) ? MUTED : r.value >= critical ? RED : r.value >= warning ? AMBER : GREEN), 0);
        }
        const bool t_ok = vm->readings[0].fresh(now, vm->settings.stale_ms), h_ok = vm->readings[1].fresh(now, vm->settings.stale_ms);
        const float t = vm->readings[0].value, h = vm->readings[1].value;
        animate_gauge(t_ok ? std::clamp(int(t * 10), -100, 450) : -100);
        lv_obj_set_style_arc_opa(gauge, t_ok ? LV_OPA_COVER : LV_OPA_30, LV_PART_INDICATOR);
        lv_bar_set_value(humid_bar, h_ok ? int(h) : 0, LV_ANIM_ON);
        const bool comfortable = t_ok && h_ok && t >= l.temp_low && t <= l.temp_high && h >= l.rh_low && h <= l.rh_high;
        set_text(comfort, !(t_ok && h_ok) ? "Sensor unavailable" : comfortable ? "Comfortable" : "Outside comfort");
        lv_obj_set_style_text_color(comfort, color(!(t_ok && h_ok) ? MUTED : comfortable ? GREEN : AMBER), 0);
        lv_obj_align(comfort, LV_ALIGN_TOP_MID, 0, 150);
        const float d = t_ok && h_ok ? dew_point(t, h) : NAN;
        if (std::isfinite(d)) std::snprintf(buffer, sizeof(buffer), "%.1f°C", double(d)); else std::strcpy(buffer, "--");
        set_text(dew, buffer);
        if (t_ok && h_ok) std::snprintf(buffer, sizeof(buffer), "%.1f g/m3", double(absolute_humidity(t, h))); else std::strcpy(buffer, "--");
        set_text(absolute, buffer);
        float values[60]; vm->history.chart(Metric::Temperature, now, window_minutes, values, 60);
        chart_values(history_chart, history_series, values, 60, 1);
        float first = NAN, last = NAN;
        for (float v : values) if (std::isfinite(v)) { if (!std::isfinite(first)) first = v; last = v; }
        if (std::isfinite(first) && std::isfinite(last)) std::snprintf(buffer, sizeof(buffer), "TEMP TREND  %+.1f°C", double(last - first));
        else std::strcpy(buffer, "TEMP TREND / NO DATA");
        set_text(trend, buffer);
        vm->history.chart(Metric::Co2, now, window_minutes, values, 24); chart_values(co2_chart, co2_series, values, 24, 100);
        vm->history.chart(Metric::Voc, now, window_minutes, values, 24); chart_values(voc_chart, voc_series, values, 24, 30);
    } else if (current_page == 1) {
        std::snprintf(buffer, sizeof(buffer), "Up %lluh %llum  /  free heap %lu KB  /  MQTT %s",
            (unsigned long long)(vm->network.uptime_ms / 3600000), (unsigned long long)(vm->network.uptime_ms / 60000 % 60),
            (unsigned long)(vm->network.free_heap / 1024), vm->network.mqtt_connected ? "on" : "off");
        set_text(subtitle, buffer);
        const auto& n = vm->network;
        if (n.connected) std::snprintf(buffer, sizeof(buffer), "%s   /   %d dBm\nIP      %s\nGW   %s     DNS   %s", n.ssid, n.rssi, n.ip, n.gateway, n.dns);
        else std::snprintf(buffer, sizeof(buffer), "%s\nWi-Fi disconnected\nTap here to configure connectivity", n.ssid);
        set_text(network_info, buffer);
        for (size_t i = 0; i < 4; ++i) {
            const auto& host = n.hosts[i]; const auto& cfg = vm->settings.hosts[i];
            const bool stale = !n.connected || now < host.last_ok_ms || (host.last_ok_ms && now - host.last_ok_ms > 60000);
            const uint32_t c = !cfg.address[0] || !host.checked || stale ? MUTED : host.online ? GREEN : RED;
            lv_obj_set_style_bg_color(host_leds[i], color(c), 0);
            if (!cfg.address[0]) std::snprintf(buffer, sizeof(buffer), "Host %u / tap to configure", unsigned(i + 1));
            else if (!n.connected || !host.checked) std::snprintf(buffer, sizeof(buffer), "%s  /  unknown", cfg.name);
            else if (host.online && !stale) std::snprintf(buffer, sizeof(buffer), "%s  /  %.0f ms", cfg.name, double(host.latency_ms));
            else std::snprintf(buffer, sizeof(buffer), "%s  /  %s", cfg.name, stale ? "stale" : "offline");
            set_text(host_labels[i], buffer); lv_obj_set_style_text_color(host_labels[i], color(c), 0);
            float points[32]; std::fill(points, points + 32, NAN);
            for (unsigned k = 0; k < host.count; ++k)
                points[32 - host.count + k] = host.history[(host.cursor + 32 - host.count + k) % 32];
            chart_values(host_charts[i], host_series[i], points, 32, 20);
        }
    } else if (current_page==3) {
        const bool fresh=vm->lora.connected && vm->lora.last_message_ms && now>=vm->lora.last_message_ms && now-vm->lora.last_message_ms<=vm->settings.sniffer.timeout_ms;
        set_text(status,vm->simulated?"SIMULATED":fresh?"CONNECTED":vm->lora.enabled?"CONNECTING":"DISABLED");
        lv_obj_set_style_text_color(status,color(fresh?GREEN:MUTED),0);
        set_text(subtitle,vm->lora.diagnostic); lora_page::refresh(now);
    } else set_text(subtitle, "Optional integration / no active data source");
}
namespace widgets {
lv_obj_t* make_panel(lv_obj_t* p,int x,int y,int w,int h) { return panel(p,x,y,w,h); }
lv_obj_t* label(lv_obj_t* p,const char* s,int x,int y,const lv_font_t* f,uint32_t c) { return text(p,s,x,y,f,c); }
void label_update(lv_obj_t* o,const char* s) { set_text(o,s); }
lv_obj_t* action(lv_obj_t* p,const char* s,int x,int y,int w,int h,lv_event_cb_t cb,void* data) { return button(p,s,x,y,w,h,cb,data); }
lv_obj_t* line_chart(lv_obj_t* p,int x,int y,int w,int h,lv_chart_series_t** s,uint32_t c,unsigned n) { return chart(p,x,y,w,h,s,c,n); }
void set_values(lv_obj_t* o,lv_chart_series_t* s,const float* v,size_t n,float span) { chart_values(o,s,v,n,span); }
lv_obj_t* detail(const char* title) {
    if (modal) return nullptr;
    open_modal(12,60,456,356);
    text(modal,title,20,20,&lv_font_montserrat_24);
    button(modal,LV_SYMBOL_CLOSE,390,12,48,44,close_modal); return modal;
}
}
} // namespace dash::ui
