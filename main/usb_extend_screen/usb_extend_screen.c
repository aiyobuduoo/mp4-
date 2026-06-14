#include "usb_extend_screen.h"

#include "esp_check.h"
#include "esp_log.h"
#include "lvgl_demo.h"
#include "usb_extend_screen_internal.h"

static const char *TAG = "usb_extend_screen";
static bool s_initialized;
static volatile bool s_active;

bool usb_extend_screen_active_internal(void)
{
    return s_active;
}

bool usb_extend_screen_is_active(void)
{
    return s_active;
}

esp_err_t usb_extend_screen_start(void)
{
    if (s_active) {
        return ESP_OK;
    }

    if (!s_initialized) {
        ESP_RETURN_ON_ERROR(usb_extend_frame_pool_init(), TAG, "frame pool init failed");
        ESP_RETURN_ON_ERROR(usb_extend_display_init(), TAG, "display init failed");
        ESP_RETURN_ON_ERROR(usb_extend_vendor_init(), TAG, "vendor init failed");
        ESP_RETURN_ON_ERROR(usb_extend_hid_init(), TAG, "HID touch init failed");
        ESP_RETURN_ON_ERROR(usb_extend_usb_init(), TAG, "USB init failed");
        s_initialized = true;
    }

    lvgl_demo_set_paused(true);
    usb_extend_vendor_reset();
    s_active = true;
    usb_extend_usb_connect();
    ESP_LOGI(TAG, "USB extend screen enabled");
    return ESP_OK;
}

esp_err_t usb_extend_screen_stop(void)
{
    if (!s_active) {
        return ESP_OK;
    }

    s_active = false;
    usb_extend_usb_disconnect();
    usb_extend_vendor_reset();
    usb_extend_display_wait_idle();
    lvgl_demo_set_paused(false);
    ESP_LOGI(TAG, "USB extend screen disabled");
    return ESP_OK;
}
