#pragma once
#include "core/model.hpp"
namespace dash::ui {
void create(ViewModel& model);
void refresh(uint64_t now);
void navigate(unsigned page);
// Called only on the LVGL-owning task after a physical user-button request.
void toggle_screen();
}
