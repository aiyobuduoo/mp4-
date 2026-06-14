#include <stdio.h>
#include "es8311_code.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "MP3.h"
#include "sd.h"
#include "lcd.h"
#include "lvgl_demo.h"
#include "wifi.h"
#include "ntp.h"
#include "weather.h"
#include "history_today.h"
#include "memo.h"

static const char *TAG = "main";

static void wifi_task(void *arg)
{
    (void)arg;
    if (wifi_init_sta()) {
        if (!ntp_sync_from_aliyun()) {
            ESP_LOGW(TAG, "NTP synchronization failed, continuing with main UI");
        }
        esp_err_t weather_err = weather_task_start();
        if (weather_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start weather task: %s", esp_err_to_name(weather_err));
        }
        esp_err_t history_err = history_today_task_start();
        if (history_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start history task: %s", esp_err_to_name(history_err));
        }
        esp_err_t memo_err = memo_service_start();
        if (memo_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start memo web service: %s", esp_err_to_name(memo_err));
        } else {
            char device_ip[16] = {0};
            if (wifi_get_ip_address(device_ip, sizeof(device_ip))) {
                ESP_LOGI(TAG, "Device IP address: %s", device_ip);
                ESP_LOGI(TAG, "Memo webpage: http://%s:%u/",
                         device_ip, (unsigned)memo_service_port());
            }
        }
        lvgl_demo_enter_main_ui();
    } else {
        ESP_LOGE(TAG, "Wi-Fi initialization failed");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_LOGI(TAG, "Initializing ES8311 codec");
    ESP_ERROR_CHECK(es8311_codec_init());
    ESP_LOGI(TAG, "ES8311 codec initialized successfully");
    es8311_codec_set_volume(5);
    mp3_init();

    if (!sdcard_try_init()) {
        ESP_LOGE(TAG, "SD card mount failed: %s", sdcard_last_debug());
    } else {
        ESP_LOGI(TAG, "SD card mounted, scanning /sdcard/music");
       // list_sdcard_files("/sdcard/music");
       // mp3_play_first_file_in_dir("/sdcard/music");
    }

    ESP_LOGI(TAG, "Initializing LCD");
    lcd_init();
    ESP_LOGI(TAG, "LCD initialized, backlight=%d%%", lcd_get_backlight_percent());

    ESP_LOGI(TAG, "Starting LVGL demo");
    lvgl_demo_init();

#if !APP_SKIP_WIFI_INIT
    if (xTaskCreate(wifi_task, "wifi_init", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create Wi-Fi initialization task");
    }
#else
    lvgl_demo_enter_main_ui();
#endif
}
