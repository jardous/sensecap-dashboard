#include "ui/dashboard.hpp"
#include "app/platform.hpp"
#include "lvgl.h"
#include <SDL.h>
#include <array>
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
static dash::ViewModel model;
namespace dash::platform {
uint64_t now_ms() { return clock_ms; }
bool save_settings(const Settings& s) { model.settings = s; return true; } // RAM only, no credentials on disk
void apply_backlight(uint8_t) {}
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
static void click_label(const char* label) {
    auto* o = find_text(lv_scr_act(), label);
    if (!o) { std::fprintf(stderr, "UI test missing label: %s\n", label); std::exit(1); }
    lv_event_send(lv_obj_get_parent(o), LV_EVENT_CLICKED, nullptr);
}
static void self_test() {
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
    }
    clock_ms += 300000; dash::ui::refresh(clock_ms);
    std::puts("UI: navigation, details, settings, keyboard and stale states passed");
}
int main(int argc, char** argv) {
    bool simulated = false, unavailable = false, test = false;
    std::string screenshot;
    unsigned page = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--simulate")) simulated = true;
        else if (!std::strcmp(argv[i], "--self-test")) test = true;
        else if (!std::strcmp(argv[i], "--unavailable")) unavailable = true;
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--page") && i + 1 < argc) page = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else { std::fprintf(stderr, "Usage: simulator [--simulate|--unavailable] [--page 0..3] [--screenshot image.ppm] [--self-test]\n"); return 2; }
    }
    if (page > 3 || (simulated && unavailable)) return 2;
    if (simulated) simulate();
    lv_init();
    lv_disp_draw_buf_t buffer; lv_disp_draw_buf_init(&buffer, draw_buffer.data(), nullptr, draw_buffer.size());
    lv_disp_drv_t display; lv_disp_drv_init(&display); display.hor_res = 480; display.ver_res = 480;
    display.draw_buf = &buffer; display.flush_cb = flush; lv_disp_drv_register(&display);
    lv_indev_drv_t pointer; lv_indev_drv_init(&pointer); pointer.type = LV_INDEV_TYPE_POINTER; pointer.read_cb = input;
    lv_indev_drv_register(&pointer);
    dash::ui::create(model); dash::ui::navigate(page);
    if (test) { self_test(); return 0; }
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
                for (auto& h : model.network.hosts) if (h.online) h.last_ok_ms = clock_ms;
            }
            dash::ui::refresh(clock_ms); last_refresh = now;
        }
        lv_timer_handler(); SDL_UpdateTexture(texture, nullptr, pixels.data(), 480 * sizeof(uint32_t));
        SDL_RenderCopy(renderer, texture, nullptr, nullptr); SDL_RenderPresent(renderer); SDL_Delay(5);
    }
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
}
