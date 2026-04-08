#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_icon_battery;
extern const lv_img_dsc_t img_icon_brake;
extern const lv_img_dsc_t img_icon_foglight;
extern const lv_img_dsc_t img_icon_fuel;
extern const lv_img_dsc_t img_icon_glowplug;
extern const lv_img_dsc_t img_icon_highbeam;
extern const lv_img_dsc_t img_icon_oil;
extern const lv_img_dsc_t img_icon_temp;
extern const lv_img_dsc_t img_icon_warning;
extern const lv_img_dsc_t img_icon_blinker_l;
extern const lv_img_dsc_t img_icon_blinker_r;
extern const lv_img_dsc_t img_speedmask;
extern const lv_img_dsc_t img_temp_indicator;
extern const lv_img_dsc_t img_glowplug_notification;
extern const lv_img_dsc_t img_led;
extern const lv_img_dsc_t img_pointgrid;
extern const lv_img_dsc_t img_icon_wifi;
extern const lv_img_dsc_t img_speedogrid;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[18];


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/