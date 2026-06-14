#include "lvgl_demo.h"

#include <assert.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>

#include "config.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_envent.h"
#include "time_ui.h"
#include "usb_extend_screen_ui.h"

static const char *TAG = "lvgl_demo";

#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_STACK_SIZE   (6 * 1024)
#define LVGL_TASK_PRIORITY     2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 2
#define LVGL_DRAW_BUF_LINES    80

static _lock_t s_lvgl_api_lock;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_touch_drv;
static lv_indev_t *s_touch_indev;
static lv_disp_t *s_display;
static esp_timer_handle_t s_lvgl_tick_timer;
static lv_obj_t *s_splash_screen;
static bool s_main_ui_loaded;
static volatile bool s_lvgl_paused;
static volatile bool s_lvgl_refresh_pending;

static lv_obj_t *show_splash_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    lv_obj_t *image = lv_img_create(screen);
    lv_img_set_src(image, &ui_img_1230129726);
    lv_obj_center(image);

    lv_scr_load(screen);
    lv_refr_now(s_display);
    return screen;
}

static void lvgl_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)disp_drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static bool lcd_flush_ready_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_drv);
    return false;
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_touch_read_cb(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)indev_drv->user_data;
    esp_lcd_touch_point_data_t touch_data[1] = {0};
    uint8_t touch_count = 0;

    esp_lcd_touch_read_data(touch);
    esp_err_t err = esp_lcd_touch_get_data(touch, touch_data, &touch_count, 1);
    if (err == ESP_OK && touch_count > 0) {
        data->point.x = touch_data[0].x;
        data->point.y = touch_data[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    while (1) {
        if (s_lvgl_paused) {
            usleep(20 * 1000);
            continue;
        }
        _lock_acquire(&s_lvgl_api_lock);
        if (s_lvgl_refresh_pending && s_display != NULL) {
            s_lvgl_refresh_pending = false;
            lv_obj_invalidate(lv_scr_act());
            lv_refr_now(s_display);
        }
        uint32_t delay_ms = lv_timer_handler();
        _lock_release(&s_lvgl_api_lock);

        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(delay_ms * 1000);
    }
}

void lvgl_demo_set_paused(bool paused)
{
    s_lvgl_paused = paused;
    if (!paused) {
        s_lvgl_refresh_pending = true;
    }
    ESP_LOGI(TAG, "LVGL rendering %s", paused ? "paused" : "resumed");
}

void lvgl_demo_run_async(void (*callback)(void *), void *user_data)
{
    if (callback == NULL) {
        return;
    }
    _lock_acquire(&s_lvgl_api_lock);
    lv_async_call(callback, user_data);
    _lock_release(&s_lvgl_api_lock);
}

void lvgl_demo_init(void)
{
    if (s_display != NULL) {
        ESP_LOGW(TAG, "LVGL demo already initialized");
        return;
    }

    esp_lcd_panel_handle_t panel = lcd_get_panel_handle();
    if (panel == NULL) {
        ESP_LOGE(TAG, "LCD panel is not initialized");
        return;
    }

    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();

    size_t draw_buffer_pixels = BSP_LCD_H_RES * LVGL_DRAW_BUF_LINES;
    size_t draw_buffer_size = draw_buffer_pixels * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_calloc(1, draw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_calloc(1, draw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(buf1);
    assert(buf2);

    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, draw_buffer_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BSP_LCD_H_RES;
    s_disp_drv.ver_res = BSP_LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = panel;
    s_disp_drv.sw_rotate = 1;
    s_disp_drv.rotated = LV_DISP_ROT_90;
    s_display = lv_disp_drv_register(&s_disp_drv);
    assert(s_display);
    ESP_LOGI(TAG, "LVGL landscape display ready: %dx%d",
             lv_disp_get_hor_res(s_display), lv_disp_get_ver_res(s_display));

    esp_lcd_touch_handle_t touch = lcd_get_touch_handle();
    if (touch != NULL) {
        lv_indev_drv_init(&s_touch_drv);
        s_touch_drv.type = LV_INDEV_TYPE_POINTER;
        s_touch_drv.disp = s_display;
        s_touch_drv.read_cb = lvgl_touch_read_cb;
        s_touch_drv.user_data = touch;
        s_touch_indev = lv_indev_drv_register(&s_touch_drv);
        assert(s_touch_indev);
        ESP_LOGI(TAG, "LVGL touch input ready");
    } else {
        ESP_LOGW(TAG, "Touch handle is NULL, LVGL touch input disabled");
    }

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = lcd_flush_ready_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, &s_disp_drv));

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &s_lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    _lock_acquire(&s_lvgl_api_lock);
    s_splash_screen = show_splash_screen();
    _lock_release(&s_lvgl_api_lock);

    vTaskDelay(pdMS_TO_TICKS(APP_SPLASH_FRAME_SETTLE_MS));
    ESP_LOGI(TAG, "Splash frame ready, enabling backlight");
    ESP_ERROR_CHECK(lcd_set_backlight_percent(100));

    xTaskCreate(lvgl_port_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "LVGL splash ready, waiting for Wi-Fi");
}

void lvgl_demo_enter_main_ui(void)
{
    if (s_display == NULL) {
        ESP_LOGE(TAG, "Cannot enter main UI before LVGL initialization");
        return;
    }
    if (s_main_ui_loaded) {
        return;
    }

    _lock_acquire(&s_lvgl_api_lock);
    if (!s_main_ui_loaded) {
        ui_init();
        ui_events_init();
        usb_extend_screen_ui_init();
        time_ui_start();
        if (s_splash_screen != NULL) {
            lv_obj_del(s_splash_screen);
            s_splash_screen = NULL;
        }
        s_main_ui_loaded = true;
    }
    _lock_release(&s_lvgl_api_lock);

    ESP_LOGI(TAG, "Wi-Fi ready, main UI loaded");
}
