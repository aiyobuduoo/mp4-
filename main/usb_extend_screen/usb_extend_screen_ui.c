#include "usb_extend_screen_ui.h"

#include <stdint.h>
#include <stdlib.h>

#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "lvgl.h"
#include "lvgl_demo.h"
#include "ui/ui.h"
#include "usb_extend_hid.h"
#include "usb_extend_screen.h"
#include "usb_extend_screen_internal.h"

#define EXIT_SWIPE_DISTANCE 80

typedef enum {
    DIALOG_ENTER,
    DIALOG_EXIT,
} dialog_mode_t;

static const char *TAG = "usb_extend_ui";
static lv_obj_t *s_dialog;
static dialog_mode_t s_dialog_mode;
static TaskHandle_t s_touch_task;

static void close_dialog(void)
{
    if (s_dialog != NULL) {
        lv_obj_del(s_dialog);
        s_dialog = NULL;
    }
}

static void dialog_button_event(lv_event_t *event)
{
    bool confirm = (bool)(uintptr_t)lv_event_get_user_data(event);
    dialog_mode_t mode = s_dialog_mode;
    close_dialog();

    if (mode == DIALOG_ENTER && confirm) {
        esp_err_t err = usb_extend_screen_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot enter USB extend screen: %s", esp_err_to_name(err));
        }
    } else if (mode == DIALOG_EXIT && !confirm) {
        esp_err_t err = usb_extend_screen_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot continue USB extend screen: %s", esp_err_to_name(err));
        }
    }
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text, int32_t x,
                               lv_color_t color, bool confirm)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 150, 54);
    lv_obj_set_x(button, x);
    lv_obj_set_y(button, 57);
    lv_obj_set_align(button, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(button, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, color, LV_PART_MAIN);
    lv_obj_add_event_cb(button, dialog_button_event, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)confirm);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void show_dialog(dialog_mode_t mode)
{
    if (s_dialog != NULL || ui_timeui == NULL) {
        return;
    }

    s_dialog_mode = mode;
    s_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_dialog);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_70, LV_PART_MAIN);

    lv_obj_t *panel = lv_obj_create(s_dialog);
    lv_obj_set_size(panel, 470, 220);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x181C24), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x35A7FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, mode == DIALOG_ENTER ?
                      "Enter USB extended screen?" :
                      "Exit USB extended screen?");
    lv_obj_set_style_text_font(title, &ui_font_Font3, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_y(title, -47);
    lv_obj_set_align(title, LV_ALIGN_CENTER);

    if (mode == DIALOG_ENTER) {
        create_button(panel, "Cancel", -95, lv_color_hex(0x555B66), false);
        create_button(panel, "Enter", 95, lv_color_hex(0x1687F8), true);
    } else {
        create_button(panel, "Continue", -95, lv_color_hex(0x1687F8), false);
        create_button(panel, "Exit", 95, lv_color_hex(0xE34B4B), true);
    }
}

static void show_exit_dialog_async(void *arg)
{
    (void)arg;
    show_dialog(DIALOG_EXIT);
}

static void timeui_gesture_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE ||
        usb_extend_screen_is_active()) {
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM) {
        lv_indev_wait_release(indev);
        show_dialog(DIALOG_ENTER);
    }
}

static void active_touch_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    uint16_t start_x = 0;
    uint16_t start_y = 0;
    uint16_t last_x = 0;
    uint16_t last_y = 0;

    for (;;) {
        if (!usb_extend_screen_is_active()) {
            pressed = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        esp_lcd_touch_handle_t touch = lcd_get_touch_handle();
        esp_lcd_touch_point_data_t points[USB_EXTEND_TOUCH_POINTS] = {0};
        uint8_t point_count = 0;
        esp_err_t err = touch == NULL ? ESP_ERR_INVALID_STATE :
                        esp_lcd_touch_read_data(touch);
        if (err == ESP_OK) {
            err = esp_lcd_touch_get_data(touch, points, &point_count,
                                         USB_EXTEND_TOUCH_POINTS);
        }

        if (err == ESP_OK && point_count > 0) {
            uint16_t raw_x = points[0].x < USB_EXTEND_SCREEN_HEIGHT ?
                             points[0].x : USB_EXTEND_SCREEN_HEIGHT - 1;
            uint16_t raw_y = points[0].y < USB_EXTEND_SCREEN_WIDTH ?
                             points[0].y : USB_EXTEND_SCREEN_WIDTH - 1;
            last_x = USB_EXTEND_SCREEN_WIDTH - raw_y - 1;
            last_y = raw_x;
            if (!pressed) {
                pressed = true;
                start_x = last_x;
                start_y = last_y;
            }
            usb_extend_hid_send_touch(points, point_count);
        } else if (pressed) {
            int32_t dx = (int32_t)last_x - start_x;
            int32_t dy = (int32_t)last_y - start_y;
            pressed = false;
            usb_extend_hid_send_release();
            if (dy >= EXIT_SWIPE_DISTANCE && abs(dy) > abs(dx)) {
                ESP_LOGI(TAG, "Down swipe detected, asking to exit extend screen");
                usb_extend_screen_stop();
                lvgl_demo_run_async(show_exit_dialog_async, NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void usb_extend_screen_ui_init(void)
{
    if (ui_timeui != NULL) {
        lv_obj_add_event_cb(ui_timeui, timeui_gesture_event, LV_EVENT_GESTURE, NULL);
    }
    if (s_touch_task == NULL &&
        xTaskCreate(active_touch_task, "usb_ext_touch", 3072, NULL, 4,
                    &s_touch_task) != pdPASS) {
        s_touch_task = NULL;
        ESP_LOGE(TAG, "Cannot create extend-screen touch task");
    }
}
