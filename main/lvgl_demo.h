#ifndef LVGL_DEMO_H
#define LVGL_DEMO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void lvgl_demo_init(void);
void lvgl_demo_enter_main_ui(void);
void lvgl_demo_set_paused(bool paused);
void lvgl_demo_run_async(void (*callback)(void *), void *user_data);

#ifdef __cplusplus
}
#endif

#endif
