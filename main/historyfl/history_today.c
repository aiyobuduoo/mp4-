#include "history_today.h"

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

#define HISTORY_TODAY_API_URL "https://uapis.cn/api/v1/history/programmer/today"
#define HISTORY_TODAY_HTTP_TIMEOUT_MS 20000
#define HISTORY_TODAY_RESPONSE_MAX_LEN 4096
#define HISTORY_TODAY_TASK_STACK_SIZE 8192
#define HISTORY_TODAY_TASK_PRIORITY 3
#define HISTORY_TODAY_INITIAL_DELAY_MS 30000
#define HISTORY_TODAY_UPDATE_INTERVAL_MS (24ULL * 60ULL * 60ULL * 1000ULL)

typedef struct {
    char data[HISTORY_TODAY_RESPONSE_MAX_LEN];
    size_t length;
    bool overflow;
} history_http_response_t;

static const char *TAG = "history_today";
static TaskHandle_t s_history_task;
static SemaphoreHandle_t s_history_mutex;
static char s_latest_text[HISTORY_TODAY_TEXT_MAX_LEN];
static bool s_has_latest_text;

static esp_err_t history_http_event_handler(esp_http_client_event_t *event)
{
    history_http_response_t *response = (history_http_response_t *)event->user_data;
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

static esp_err_t history_today_fetch(char *out_text, size_t out_size)
{
    history_http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = HISTORY_TODAY_API_URL,
        .event_handler = history_http_event_handler,
        .user_data = &response,
        .timeout_ms = HISTORY_TODAY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32P4-History/1.0");

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    if (status_code != 200 || response.overflow || response.length == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(response.data);
    cJSON *events = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "events") : NULL;
    if (!cJSON_IsArray(events) || cJSON_GetArraySize(events) == 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *selected = NULL;
    int selected_importance = -1;
    cJSON *event = NULL;
    cJSON_ArrayForEach(event, events) {
        cJSON *importance = cJSON_GetObjectItemCaseSensitive(event, "importance");
        int value = cJSON_IsNumber(importance) ? importance->valueint : 0;
        if (selected == NULL || value > selected_importance) {
            selected = event;
            selected_importance = value;
        }
    }

    cJSON *year = cJSON_GetObjectItemCaseSensitive(selected, "year");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(selected, "title");
    if (!cJSON_IsNumber(year) || !cJSON_IsString(title) || title->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int written = snprintf(out_text, out_size, "%d - %s", year->valueint, title->valuestring);
    cJSON_Delete(root);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void history_today_store(const char *text)
{
    if (text == NULL || s_history_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_history_mutex, portMAX_DELAY) == pdTRUE) {
        snprintf(s_latest_text, sizeof(s_latest_text), "%s", text);
        s_has_latest_text = true;
        xSemaphoreGive(s_history_mutex);
    }
}

bool history_today_get_latest(char *out_text, size_t out_size)
{
    if (out_text == NULL || out_size == 0 || s_history_mutex == NULL) {
        return false;
    }

    bool available = false;
    if (xSemaphoreTake(s_history_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_has_latest_text) {
            snprintf(out_text, out_size, "%s", s_latest_text);
            available = true;
        }
        xSemaphoreGive(s_history_mutex);
    }
    return available;
}

static void history_today_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(HISTORY_TODAY_INITIAL_DELAY_MS));

    for (;;) {
        if (wifi_is_connected()) {
            char text[HISTORY_TODAY_TEXT_MAX_LEN] = {0};
            esp_err_t err = history_today_fetch(text, sizeof(text));
            if (err == ESP_OK) {
                history_today_store(text);
                ESP_LOGI(TAG, "History updated: %s", text);
            } else {
                ESP_LOGW(TAG, "History request failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Skipping history update: Wi-Fi disconnected");
        }

        vTaskDelay(pdMS_TO_TICKS(HISTORY_TODAY_UPDATE_INTERVAL_MS));
    }
}

esp_err_t history_today_task_start(void)
{
    if (s_history_task != NULL) {
        return ESP_OK;
    }

    if (s_history_mutex == NULL) {
        s_history_mutex = xSemaphoreCreateMutex();
        if (s_history_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(history_today_task, "history_today", HISTORY_TODAY_TASK_STACK_SIZE,
                    NULL, HISTORY_TODAY_TASK_PRIORITY, &s_history_task) != pdPASS) {
        s_history_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "History task started: first update in %d ms, then every 24 hours",
             HISTORY_TODAY_INITIAL_DELAY_MS);
    return ESP_OK;
}
