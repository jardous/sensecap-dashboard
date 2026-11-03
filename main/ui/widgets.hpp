#pragma once
#include "lvgl.h"
namespace dash::ui::widgets {
inline constexpr uint32_t background=0x0B1018,card=0x17212D,cyan=0x45D9E8,green=0x49DEAC,amber=0xFFC16B,red=0xFF7D84,white=0xEDF5FA,muted=0x96ABBE;
lv_obj_t* make_panel(lv_obj_t*,int,int,int,int);
lv_obj_t* label(lv_obj_t*,const char*,int,int,const lv_font_t*,uint32_t=white);
void label_update(lv_obj_t*,const char*);
lv_obj_t* action(lv_obj_t*,const char*,int,int,int,int,lv_event_cb_t,void* = nullptr);
lv_obj_t* line_chart(lv_obj_t*,int,int,int,int,lv_chart_series_t**,uint32_t,unsigned);
void set_values(lv_obj_t*,lv_chart_series_t*,const float*,size_t,float);
lv_obj_t* detail(const char* title); // shares existing modal/navigation exclusion and close handling
}
