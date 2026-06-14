#include "time_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "history_today.h"
#include "memo.h"
#include "ui.h"
#include "weather.h"
#include "wifi.h"

#define TIME_UI_UPDATE_INTERVAL_MS 1000

static lv_timer_t *s_time_ui_timer;
static char s_last_weather_report[WEATHER_REPORT_TIME_MAX_LEN];
static bool s_weather_ui_initialized;
static lv_obj_t *s_history_label;
static char s_last_history_text[HISTORY_TODAY_TEXT_MAX_LEN];
static char s_last_memo_text[MEMO_TEXT_MAX_LEN];
static bool s_memo_ui_initialized;

static void set_hidden(lv_obj_t *object, bool hidden)
{
    if (object == NULL) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static const lv_img_dsc_t *weather_icon_image(const char *weather_icon)
{
    int code = weather_icon != NULL ? atoi(weather_icon) : 0;

    if (code == 100) {
        return &ui_img_1650588022;
    }
    if (code >= 302 && code <= 304) {
        return &ui_img_1840017345;
    }
    if (code >= 300 && code < 400) {
        return &ui_img_2051629593;
    }
    if (code >= 400 && code < 500) {
        return &ui_img_1889690653;
    }
    if (code == 500 || code == 501 || code == 509 || code == 510 ||
        code == 514 || code == 515) {
        return &ui_img_701658425;
    }
    if (code >= 502 && code < 600) {
        return &ui_img_46488613;
    }
    return &ui_img_1772208564;
}

static void update_weather_ui(void)
{
    weather_info_t weather = {0};
    if (!weather_get_latest(&weather)) {
        return;
    }
    if (s_weather_ui_initialized &&
        strcmp(s_last_weather_report, weather.report_time) == 0) {
        return;
    }

    char temperature_text[16] = {0};
    char humidity_text[16] = {0};
    snprintf(temperature_text, sizeof(temperature_text), "%d", weather.temperature);
    snprintf(humidity_text, sizeof(humidity_text), "%d", weather.humidity);

    if (ui_weatext != NULL) {
        lv_label_set_text(ui_weatext, weather.weather);
    }
    if (ui_Label12 != NULL) {
        lv_label_set_text(ui_Label12, weather.city);
    }
    if (ui_wendu != NULL) {
        lv_label_set_text(ui_wendu, temperature_text);
    }
    if (ui_shidutext != NULL) {
        lv_label_set_text(ui_shidutext, humidity_text);
    }
    if (ui_weaicon != NULL) {
        lv_img_set_src(ui_weaicon, weather_icon_image(weather.weather_icon));
    }

    snprintf(s_last_weather_report, sizeof(s_last_weather_report), "%s", weather.report_time);
    s_weather_ui_initialized = true;
}

static void update_history_ui(void)
{
    char history_text[HISTORY_TODAY_TEXT_MAX_LEN] = {0};
    if (!history_today_get_latest(history_text, sizeof(history_text))) {
        return;
    }

    if (s_history_label == NULL && ui_elseev != NULL) {
        s_history_label = lv_label_create(ui_elseev);
        lv_obj_set_width(s_history_label, 480);
        lv_obj_set_align(s_history_label, LV_ALIGN_CENTER);
        lv_label_set_long_mode(s_history_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(s_history_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_history_label, lv_color_hex(0xFDFDFD), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_history_label, &ui_font_Font5, LV_PART_MAIN);
    }
    if (s_history_label == NULL || strcmp(s_last_history_text, history_text) == 0) {
        return;
    }

    lv_label_set_text(s_history_label, history_text);
    snprintf(s_last_history_text, sizeof(s_last_history_text), "%s", history_text);
}

static void update_memo_ui(void)
{
    char memo_text[MEMO_TEXT_MAX_LEN] = {0};
    if (!memo_get_latest(memo_text, sizeof(memo_text)) || ui_SHIJIAN == NULL) {
        return;
    }
    if (s_memo_ui_initialized && strcmp(s_last_memo_text, memo_text) == 0) {
        return;
    }

    lv_obj_set_width(ui_SHIJIAN, 190);
    lv_label_set_long_mode(ui_SHIJIAN, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui_SHIJIAN, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui_SHIJIAN, memo_text[0] != '\0' ? memo_text : "No memo");
    snprintf(s_last_memo_text, sizeof(s_last_memo_text), "%s", memo_text);
    s_memo_ui_initialized = true;
}

static void time_ui_update(lv_timer_t *timer)
{
    (void)timer;

    time_t now = 0;
    struct tm timeinfo = {0};
    char clock_text[16] = {0};
    char date_text[32] = {0};
    static const char *week_text[] = {
        "\xE6\x98\x9F\xE6\x9C\x9F\xE6\x97\xA5",
        "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xB8\x80",
        "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xBA\x8C",
        "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xB8\x89",
        "\xE6\x98\x9F\xE6\x9C\x9F\xE5\x9B\x9B",
        "\xE6\x98\x9F\xE6\x9C\x9F\xE4\xBA\x94",
        "\xE6\x98\x9F\xE6\x9C\x9F\xE5\x85\xAD"
    };

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(clock_text, sizeof(clock_text), "%H:%M", &timeinfo);
    strftime(date_text, sizeof(date_text),
             "%Y\xE5\xB9\xB4%m\xE6\x9C\x88%d\xE6\x97\xA5",
             &timeinfo);

    if (ui_time != NULL) {
        lv_label_set_text(ui_time, clock_text);
    }
    if (ui_timelable != NULL) {
        lv_label_set_text(ui_timelable, clock_text);
    }
    if (ui_data1 != NULL) {
        lv_label_set_text(ui_data1, date_text);
    }
    if (ui_week != NULL && timeinfo.tm_wday >= 0 && timeinfo.tm_wday < 7) {
        lv_label_set_text(ui_week, week_text[timeinfo.tm_wday]);
    }

    bool connected = wifi_is_connected();
    set_hidden(ui_wifi1, !connected);
    set_hidden(ui_wifi2, connected);
    update_weather_ui();
    update_history_ui();
    update_memo_ui();
}

void time_ui_start(void)
{
    if (s_time_ui_timer != NULL) {
        return;
    }

    time_ui_update(NULL);
    s_time_ui_timer = lv_timer_create(time_ui_update, TIME_UI_UPDATE_INTERVAL_MS, NULL);
}
