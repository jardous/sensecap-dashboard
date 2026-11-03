#pragma once
#include "core/model.hpp"
#include "lvgl.h"
namespace dash::ui::lora_page {
void create(lv_obj_t* parent,ViewModel& model);
void refresh(uint64_t now);
}
