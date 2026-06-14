#include "usb_extend_screen_internal.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb_extend_usb";
static usb_phy_handle_t s_phy;
static TaskHandle_t s_usb_task;

static void usb_task(void *arg)
{
    (void)arg;
    for (;;) {
        tud_task();
    }
}

esp_err_t usb_extend_usb_init(void)
{
    if (s_usb_task != NULL) {
        return ESP_OK;
    }

    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
    };
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_config, &s_phy), TAG, "USB PHY init failed");
    if (!tusb_init()) {
        return ESP_FAIL;
    }
    if (xTaskCreate(usb_task, "usb_ext_tusb", 4096, NULL, 6, &s_usb_task) != pdPASS) {
        s_usb_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    tud_disconnect();
    return ESP_OK;
}

void usb_extend_usb_connect(void)
{
    tud_connect();
}

void usb_extend_usb_disconnect(void)
{
    tud_disconnect();
}

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB extend screen mounted");
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB extend screen unmounted");
}
