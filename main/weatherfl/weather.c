#include "weather.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi.h"

#define WEATHER_API_URL          "https://uapis.cn/api/v1/misc/weather"
#define WEATHER_HTTP_TIMEOUT_MS  20000
#define WEATHER_RESPONSE_MAX_LEN 2048
#define WEATHER_URL_MAX_LEN      256
#define WEATHER_TASK_STACK_SIZE  8192
#define WEATHER_TASK_PRIORITY    3
#define WEATHER_INITIAL_DELAY_MS 3000
#define WEATHER_UPDATE_INTERVAL_MS (8ULL * 60ULL * 60ULL * 1000ULL)

typedef struct {
    char data[WEATHER_RESPONSE_MAX_LEN];
    size_t length;
    bool overflow;
} weather_http_response_t;

static const char *TAG = "weather";
static TaskHandle_t s_weather_task;
static SemaphoreHandle_t s_weather_mutex;
static weather_info_t s_latest_weather;
static bool s_has_latest_weather;

static void copy_json_string(cJSON *root, const char *name, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(out, out_size, "%s", item->valuestring);
    }
}

static bool parse_json_integer(cJSON *root, const char *name, int *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valueint;
    return true;
}

static bool is_url_unreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static bool url_encode(const char *source, char *encoded, size_t encoded_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t position = 0;

    if (source == NULL || encoded == NULL || encoded_size == 0) {
        return false;
    }

    while (*source != '\0') {
        unsigned char ch = (unsigned char)*source++;
        if (is_url_unreserved(ch)) {
            if (position + 1 >= encoded_size) {
                return false;
            }
            encoded[position++] = (char)ch;
        } else {
            if (position + 3 >= encoded_size) {
                return false;
            }
            encoded[position++] = '%';
            encoded[position++] = hex[ch >> 4];
            encoded[position++] = hex[ch & 0x0f];
        }
    }

    encoded[position] = '\0';
    return true;
}

static esp_err_t weather_http_event_handler(esp_http_client_event_t *event)
{
    weather_http_response_t *response = (weather_http_response_t *)event->user_data;

    if (event->event_id != HTTP_EVENT_ON_DATA || response == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    size_t available = sizeof(response->data) - response->length - 1;
    size_t copy_length = (size_t)event->data_len;
    if (copy_length > available) {
        copy_length = available;
        response->overflow = true;
    }
    if (copy_length > 0) {
        memcpy(response->data + response->length, event->data, copy_length);
        response->length += copy_length;
        response->data[response->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t parse_weather_response(const char *json_text, weather_info_t *out_weather)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        ESP_LOGE(TAG, "Cannot parse weather JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out_weather, 0, sizeof(*out_weather));
    copy_json_string(root, "province", out_weather->province, sizeof(out_weather->province));
    copy_json_string(root, "city", out_weather->city, sizeof(out_weather->city));
    copy_json_string(root, "adcode", out_weather->adcode, sizeof(out_weather->adcode));
    copy_json_string(root, "weather", out_weather->weather, sizeof(out_weather->weather));
    copy_json_string(root, "weather_icon", out_weather->weather_icon, sizeof(out_weather->weather_icon));
    copy_json_string(root, "wind_direction", out_weather->wind_direction, sizeof(out_weather->wind_direction));
    copy_json_string(root, "wind_power", out_weather->wind_power, sizeof(out_weather->wind_power));
    copy_json_string(root, "report_time", out_weather->report_time, sizeof(out_weather->report_time));

    bool valid = out_weather->city[0] != '\0' &&
                 out_weather->weather[0] != '\0' &&
                 parse_json_integer(root, "temperature", &out_weather->temperature) &&
                 parse_json_integer(root, "humidity", &out_weather->humidity);
    cJSON_Delete(root);

    if (!valid) {
        ESP_LOGE(TAG, "Weather JSON is missing required fields");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t weather_fetch_url(const char *url, weather_info_t *out_weather)
{
    if (url == NULL || out_weather == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    weather_http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = weather_http_event_handler,
        .user_data = &response,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32P4-Weather/1.0");

    ESP_LOGI(TAG, "Requesting weather: %s", url);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Weather request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Weather server returned HTTP %d", status_code);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (response.overflow || response.length == 0) {
        ESP_LOGE(TAG, "Weather response is empty or too large");
        return ESP_ERR_INVALID_SIZE;
    }

    err = parse_weather_response(response.data, out_weather);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Weather: %s %s, %d C, humidity %d%%",
                 out_weather->city, out_weather->weather,
                 out_weather->temperature, out_weather->humidity);
    }
    return err;
}

static esp_err_t weather_fetch_with_parameter(const char *name, const char *value,
                                              weather_info_t *out_weather)
{
    if (name == NULL || value == NULL || value[0] == '\0' || out_weather == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char encoded_value[WEATHER_URL_MAX_LEN] = {0};
    char url[WEATHER_URL_MAX_LEN] = {0};
    if (!url_encode(value, encoded_value, sizeof(encoded_value))) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written = snprintf(url, sizeof(url), "%s?%s=%s", WEATHER_API_URL, name, encoded_value);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return weather_fetch_url(url, out_weather);
}

esp_err_t weather_fetch_by_city(const char *city, weather_info_t *out_weather)
{
    return weather_fetch_with_parameter("city", city, out_weather);
}

esp_err_t weather_fetch_by_adcode(const char *adcode, weather_info_t *out_weather)
{
    return weather_fetch_with_parameter("adcode", adcode, out_weather);
}

esp_err_t weather_fetch_by_ip(weather_info_t *out_weather)
{
    return weather_fetch_url(WEATHER_API_URL, out_weather);
}

static void weather_store_latest(const weather_info_t *weather)
{
    if (weather == NULL || s_weather_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_weather_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(&s_latest_weather, weather, sizeof(s_latest_weather));
        s_has_latest_weather = true;
        xSemaphoreGive(s_weather_mutex);
    }
}

bool weather_get_latest(weather_info_t *out_weather)
{
    if (out_weather == NULL || s_weather_mutex == NULL) {
        return false;
    }

    bool available = false;
    if (xSemaphoreTake(s_weather_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_has_latest_weather) {
            memcpy(out_weather, &s_latest_weather, sizeof(*out_weather));
            available = true;
        }
        xSemaphoreGive(s_weather_mutex);
    }
    return available;
}

static void weather_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WEATHER_INITIAL_DELAY_MS));

    for (;;) {
        if (wifi_is_connected()) {
            weather_info_t weather = {0};
            esp_err_t err = weather_fetch_by_ip(&weather);
            if (err == ESP_OK) {
                weather_store_latest(&weather);
            } else {
                ESP_LOGW(TAG, "Scheduled weather update failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Skipping scheduled weather update: Wi-Fi disconnected");
        }

        vTaskDelay(pdMS_TO_TICKS(WEATHER_UPDATE_INTERVAL_MS));
    }
}

esp_err_t weather_task_start(void)
{
    if (s_weather_task != NULL) {
        return ESP_OK;
    }

    if (s_weather_mutex == NULL) {
        s_weather_mutex = xSemaphoreCreateMutex();
        if (s_weather_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(weather_task, "weather", WEATHER_TASK_STACK_SIZE, NULL,
                    WEATHER_TASK_PRIORITY, &s_weather_task) != pdPASS) {
        s_weather_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Weather task started: first update in %d ms, then every 8 hours",
             WEATHER_INITIAL_DELAY_MS);
    return ESP_OK;
}
