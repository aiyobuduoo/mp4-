#include "ntp.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NTP_SERVER                 "ntp.aliyun.com"
#define NTP_TIMEZONE               "CST-8"
#define NTP_SYNC_TIMEOUT_MS        15000
#define NTP_SYNC_POLL_INTERVAL_MS  500

static const char *TAG = "ntp";

static bool system_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2024 - 1900);
}

bool ntp_sync_from_aliyun(void)
{
    setenv("TZ", NTP_TIMEZONE, 1);
    tzset();

    if (!esp_sntp_enabled()) {
        ESP_LOGI(TAG, "Starting SNTP server: %s", NTP_SERVER);
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, NTP_SERVER);
        esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
        esp_sntp_init();
    }

    for (int elapsed_ms = 0; elapsed_ms < NTP_SYNC_TIMEOUT_MS;
         elapsed_ms += NTP_SYNC_POLL_INTERVAL_MS) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED ||
            system_time_is_valid()) {
            time_t now = 0;
            struct tm timeinfo = {0};
            char time_text[32] = {0};

            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "Time synchronized: %s", time_text);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(NTP_SYNC_POLL_INTERVAL_MS));
    }

    ESP_LOGW(TAG, "Time synchronization timed out after %d ms", NTP_SYNC_TIMEOUT_MS);
    return false;
}
