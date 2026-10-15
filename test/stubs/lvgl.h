#pragma once
#include <stdint.h>
struct lv_obj_t { bool hidden = false; };
enum { LV_OBJ_FLAG_HIDDEN=1, LV_OBJ_FLAG_SCROLLABLE=2, LV_OPA_COVER=255, LV_ALIGN_TOP_MID=0, LV_ALIGN_CENTER=1, LV_RESULT_OK=0 };
static int qrRenders = 0;
inline lv_obj_t *lv_screen_active() { static lv_obj_t root; return &root; }
inline lv_obj_t *lv_obj_create(lv_obj_t*) { return new lv_obj_t; }
inline lv_obj_t *lv_label_create(lv_obj_t *o) { return lv_obj_create(o); }
inline lv_obj_t *lv_qrcode_create(lv_obj_t *o) { return lv_obj_create(o); }
inline void lv_obj_remove_style_all(lv_obj_t*) {}
inline void lv_obj_set_size(lv_obj_t*,int,int) {}
inline int lv_color_black() { return 0; }
inline int lv_color_white() { return 1; }
inline void lv_obj_set_style_bg_color(lv_obj_t*,int,int) {}
inline void lv_obj_set_style_bg_opa(lv_obj_t*,int,int) {}
inline void lv_obj_set_style_text_color(lv_obj_t*,int,int) {}
inline void lv_obj_align(lv_obj_t*,int,int,int) {}
inline void lv_qrcode_set_size(lv_obj_t*,int) {}
inline void lv_qrcode_set_dark_color(lv_obj_t*,int) {}
inline void lv_qrcode_set_light_color(lv_obj_t*,int) {}
inline void lv_qrcode_set_quiet_zone(lv_obj_t*,bool) {}
inline void lv_label_set_text(lv_obj_t*,const char*) {}
inline void lv_obj_add_flag(lv_obj_t *o,int flag) { if(flag==LV_OBJ_FLAG_HIDDEN) o->hidden=true; }
inline void lv_obj_remove_flag(lv_obj_t *o,int flag) { if(flag==LV_OBJ_FLAG_HIDDEN) o->hidden=false; }
inline int lv_qrcode_update(lv_obj_t*,const void*,uint32_t) { ++qrRenders; return LV_RESULT_OK; }
