#include "dashboard.hpp"
#include "app/platform.hpp"
#include "lvgl.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
lv_obj_t *modal = nullptr, *keyboard = nullptr, *settings_error = nullptr;
Settings draft;
unsigned current_page = 0;
uint32_t window_minutes = 60;
uint64_t last_rotation = 0;
bool sleeping = false;
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
void close_modal(lv_event_t*) {
    if (keyboard) { lv_obj_del(keyboard); keyboard = nullptr; }
    if (modal) { lv_obj_del(modal); modal = nullptr; }
}
void show_settings(lv_event_t*);
void details(lv_event_t* e) {
    if (modal) return;
    const auto metric = Metric(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    modal = panel(screen, 12, 60, 456, 356);
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
    lv_obj_add_event_cb(o, details, LV_EVENT_CLICKED, reinterpret_cast<void*>(size_t(metric)));
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
    clickable_metric(p, Metric::Temperature);
    lv_obj_remove_event_cb(p, details);
    lv_obj_add_event_cb(p, show_settings, LV_EVENT_CLICKED, nullptr);
    text(content, "HOST HEALTH", 20, 208, &lv_font_montserrat_14, MUTED);
    text(content, "TCP / HTTP", 349, 208, &lv_font_montserrat_14, MUTED);
    for (size_t i = 0; i < 4; ++i) {
        p = panel(content, 18, 233 + i * 45, 444, 40);
        lv_obj_set_style_radius(p, 10, 0);
        host_leds[i] = panel(p, 12, 14, 9, 9, MUTED);
        lv_obj_set_style_radius(host_leds[i], LV_RADIUS_CIRCLE, 0);
        host_labels[i] = text(p, "Not configured", 32, 10, &lv_font_montserrat_16, MUTED);
        lv_obj_set_width(host_labels[i], 324); lv_label_set_long_mode(host_labels[i], LV_LABEL_LONG_DOT);
        host_charts[i] = chart(p, 362, 9, 64, 23, &host_series[i], CYAN, 32);
        lv_obj_add_event_cb(p, [](lv_event_t* event) {
            if (modal) return;
            const size_t i = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
            const auto& cfg = vm->settings.hosts[i]; const auto& host = vm->network.hosts[i];
            modal = panel(screen, 12, 60, 456, 356);
            auto* name = text(modal, cfg.address[0] ? cfg.name : "Host not configured", 20, 20, &lv_font_montserrat_24);
            lv_obj_set_width(name, 352); lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
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
        }, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
        lv_obj_add_event_cb(p, show_settings, LV_EVENT_LONG_PRESSED, nullptr);
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
    const uint32_t inactive = lv_disp_get_inactive_time(nullptr);
    const bool sleep = vm->settings.screen_timeout_s && inactive >= vm->settings.screen_timeout_s * 1000u;
    if (sleep != sleeping) { sleeping = sleep; platform::apply_backlight(sleep ? 0 : vm->settings.brightness); }
    if (!modal && !sleep && vm->settings.rotation_s && now - last_rotation >= vm->settings.rotation_s * 1000ull)
        navigate((current_page + 1) % 2);
}
void arc_value(void* obj, int32_t value) { lv_arc_set_value(static_cast<lv_obj_t*>(obj), value); }
void animate_gauge(int value) {
    if (lv_arc_get_value(gauge) == value) return;
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, gauge); lv_anim_set_exec_cb(&a, arc_value);
    lv_anim_set_values(&a, lv_arc_get_value(gauge), value); lv_anim_set_time(&a, 450);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
}

// Settings use pointers into a separate draft; no credential ever appears in logs.
enum class FieldType { String, U32, U16, U8, Float, Check };
struct Field { void* address; size_t capacity; FieldType type; lv_obj_t* input; };
std::array<Field, 56> fields;
size_t field_count = 0;
void input_focus(lv_event_t* e) {
    if (!keyboard) {
        keyboard = lv_keyboard_create(screen); lv_obj_set_size(keyboard, 480, 210);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(keyboard, [](lv_event_t* ev) {
            if (lv_event_get_code(ev) == LV_EVENT_READY || lv_event_get_code(ev) == LV_EVENT_CANCEL) {
                lv_obj_del(keyboard); keyboard = nullptr;
                lv_obj_set_height(lv_obj_get_child(modal, 3), 316);
            }
        }, LV_EVENT_ALL, nullptr);
    }
    lv_keyboard_set_textarea(keyboard, lv_event_get_target(e));
    // Scroll form only (not the modal/header) above the software keyboard.
    auto* form = lv_obj_get_parent(lv_event_get_target(e)); lv_obj_set_height(form, 146);
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
}
void add_field(lv_obj_t* form, int& y, const char* title, void* address, FieldType type,
               size_t capacity = 0, bool secret = false) {
    text(form, title, 10, y, &lv_font_montserrat_16, MUTED);
    auto* input = lv_textarea_create(form); lv_obj_set_pos(input, 8, y + 24); lv_obj_set_size(input, 396, 43);
    lv_textarea_set_one_line(input, true); lv_textarea_set_password_mode(input, secret);
    lv_obj_set_style_bg_color(input, color(BG), 0); lv_obj_set_style_text_color(input, color(WHITE), 0);
    lv_obj_set_style_border_color(input, color(EDGE), 0);
    lv_obj_set_style_text_font(input, &lv_font_montserrat_18, 0);
    char buffer[40];
    if (type == FieldType::String) { lv_textarea_set_max_length(input, capacity - 1); lv_textarea_set_text(input, static_cast<char*>(address)); }
    else {
        if (type == FieldType::Float) std::snprintf(buffer, sizeof(buffer), "%.1f", double(*static_cast<float*>(address)));
        else std::snprintf(buffer, sizeof(buffer), "%lu", (unsigned long)(type == FieldType::U32 ? *static_cast<uint32_t*>(address) :
                     type == FieldType::U16 ? *static_cast<uint16_t*>(address) : type == FieldType::U8 ? *static_cast<uint8_t*>(address) : unsigned(*static_cast<Check*>(address))));
        lv_textarea_set_text(input, buffer); lv_textarea_set_accepted_chars(input, "0123456789.-");
        lv_textarea_set_max_length(input, 10);
    }
    lv_obj_add_event_cb(input, input_focus, LV_EVENT_FOCUSED, nullptr);
    fields[field_count++] = {address, capacity, type, input}; y += 84;
}
void save_clicked(lv_event_t*) {
    bool valid_input = true;
    for (size_t i = 0; i < field_count; ++i) {
        auto& field = fields[i]; const char* s = lv_textarea_get_text(field.input);
        if (field.type == FieldType::String) std::snprintf(static_cast<char*>(field.address), field.capacity, "%s", s);
        else {
            char* end; const float value = std::strtof(s, &end);
            if (end == s || *end || !std::isfinite(value)) { valid_input = false; break; }
            if (field.type != FieldType::Float && (value < 0 || value != std::floor(value))) { valid_input = false; break; }
            if (field.type == FieldType::Float) *static_cast<float*>(field.address) = value;
            else if (field.type == FieldType::U32 && value <= 3600000) *static_cast<uint32_t*>(field.address) = uint32_t(value);
            else if (field.type == FieldType::U16 && value <= 65535) *static_cast<uint16_t*>(field.address) = uint16_t(value);
            else if (field.type == FieldType::U8 && value <= 255) *static_cast<uint8_t*>(field.address) = uint8_t(value);
            else if (field.type == FieldType::Check && value <= 1) *static_cast<Check*>(field.address) = Check(uint8_t(value));
            else { valid_input = false; break; }
        }
    }
    const bool ok = valid_input && validate_settings(draft) && platform::save_settings(draft);
    set_text(settings_error, ok ? "Queued. Reboots after successful save." : "Invalid value. Check ranges in README.");
    lv_obj_set_style_text_color(settings_error, color(ok ? GREEN : RED), 0);
}
void show_settings(lv_event_t*) {
    if (modal) return;
    draft = vm->settings; field_count = 0;
    modal = panel(screen, 18, 12, 444, 404);
    text(modal, "Settings", 18, 14, &lv_font_montserrat_24); // child 0
    button(modal, "Save", 276, 8, 84, 44, save_clicked); // child 1
    button(modal, LV_SYMBOL_CLOSE, 374, 8, 52, 44, close_modal); // child 2
    auto* form = panel(modal, 10, 64, 424, 316, CARD); // child 3
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
    add_field(form, y, "Screen timeout (0=off, max 3600 s)", &draft.screen_timeout_s, FieldType::U16);
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
    text(form, "Optional device clients are not implemented yet.", 10, y, &lv_font_montserrat_14, MUTED); y += 36;
    add_field(form, y, "Reserved Reticulum bridge URL", draft.reticulum_url, FieldType::String, sizeof(draft.reticulum_url));
    add_field(form, y, "Reserved Reticulum token", draft.reticulum_token, FieldType::String, sizeof(draft.reticulum_token), true);
    add_field(form, y, "Reserved LoRa gateway MQTT topic", draft.lora_topic, FieldType::String, sizeof(draft.lora_topic));
    settings_error = text(form, "Local storage is not encrypted. Use trusted LAN.", 10, y + 8, &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(settings_error, 396);
}
} // namespace

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
    if (page == 0) make_climate(); else if (page == 1) make_network(); else optional_page(page == 2);
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
    } else set_text(subtitle, "Optional integration / no active data source");
}
} // namespace dash::ui
