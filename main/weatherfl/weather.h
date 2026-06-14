#ifndef APP_WEATHER_H
#define APP_WEATHER_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_TEXT_MAX_LEN 32
#define WEATHER_REPORT_TIME_MAX_LEN 32

typedef struct {
    char province[WEATHER_TEXT_MAX_LEN];
    char city[WEATHER_TEXT_MAX_LEN];
    char adcode[WEATHER_TEXT_MAX_LEN];
    char weather[WEATHER_TEXT_MAX_LEN];
    char weather_icon[WEATHER_TEXT_MAX_LEN];
    int temperature;
    char wind_direction[WEATHER_TEXT_MAX_LEN];
    char wind_power[WEATHER_TEXT_MAX_LEN];
    int humidity;
    char report_time[WEATHER_REPORT_TIME_MAX_LEN];
} weather_info_t;

esp_err_t weather_fetch_by_city(const char *city, weather_info_t *out_weather);
esp_err_t weather_fetch_by_adcode(const char *adcode, weather_info_t *out_weather);
esp_err_t weather_fetch_by_ip(weather_info_t *out_weather);
esp_err_t weather_task_start(void);
bool weather_get_latest(weather_info_t *out_weather);

#ifdef __cplusplus
}
#endif

#endif
