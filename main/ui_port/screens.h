#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *speedogrid;
    lv_obj_t *speedo;
    lv_obj_t *speedmask;
    lv_obj_t *speed;
    lv_obj_t *kmh;
    lv_obj_t *odometer;
    lv_obj_t *led1;
    lv_obj_t *led2;
    lv_obj_t *led3;
    lv_obj_t *led4;
    lv_obj_t *led5;
    lv_obj_t *led6;
    lv_obj_t *fuel_container;
    lv_obj_t *fuel_bar;
    lv_obj_t *fuel_frame;
    lv_obj_t *fuel_divider1;
    lv_obj_t *fuel_divider2;
    lv_obj_t *fuel_divider3;
    lv_obj_t *fuel_frame2;
    lv_obj_t *fuel_frame3;
    lv_obj_t *fuel_mover;
    lv_obj_t *fuel_indicator;
    lv_obj_t *fuel_value;
    lv_obj_t *fuel_icon;
    lv_obj_t *temp_container;
    lv_obj_t *temperature;
    lv_obj_t *oil_frame_hot;
    lv_obj_t *oil_frame_mid;
    lv_obj_t *oil_frame_cold;
    lv_obj_t *oil_divider1;
    lv_obj_t *oil_divider2;
    lv_obj_t *temp_frame2;
    lv_obj_t *temp_frame3;
    lv_obj_t *temp_frame4;
    lv_obj_t *temp_frame5;
    lv_obj_t *temp_frame6;
    lv_obj_t *temp_frame7;
    lv_obj_t *temp_mover;
    lv_obj_t *temp_indicator_line;
    lv_obj_t *temp_indicator;
    lv_obj_t *temp_status;
    lv_obj_t *temp_icon;
    lv_obj_t *wifi_icon;
    lv_obj_t *dashboard_failures;
    lv_obj_t *oil_icon;
    lv_obj_t *highbeam_icon;
    lv_obj_t *foglight_icon;
    lv_obj_t *brake_icon;
    lv_obj_t *glowplug_icon;
    lv_obj_t *battery_icon;
    lv_obj_t *voltage;
    lv_obj_t *blinker_l;
    lv_obj_t *blinker_l_gradient;
    lv_obj_t *blinker_r;
    lv_obj_t *blinker_r_gradient;
    lv_obj_t *notification;
    lv_obj_t *warning;
    lv_obj_t *warning_icon;
    lv_obj_t *warning_text;
    lv_obj_t *glowplug_container;
    lv_obj_t *glowplug_notification;
    lv_obj_t *glowplug_text;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_MAIN = 1,
};

void create_screen_main();
void tick_screen_main();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/