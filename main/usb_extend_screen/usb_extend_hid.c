#include "usb_extend_hid.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_extend_screen_internal.h"

static QueueHandle_t s_report_queue;
static TaskHandle_t s_hid_task;

static uint16_t clamp_coordinate(uint16_t value, uint16_t maximum)
{
    return value < maximum ? value : maximum - 1;
}

static void hid_task(void *arg)
{
    (void)arg;
    usb_extend_touch_report_t report;
    for (;;) {
        if (xQueueReceive(s_report_queue, &report, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        while (usb_extend_screen_active_internal() && !tud_hid_ready()) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (usb_extend_screen_active_internal()) {
            tud_hid_n_report(0, USB_EXTEND_REPORT_ID_TOUCH, &report, sizeof(report));
        }
    }
}

esp_err_t usb_extend_hid_init(void)
{
    if (s_hid_task != NULL) {
        return ESP_OK;
    }
    s_report_queue = xQueueCreate(1, sizeof(usb_extend_touch_report_t));
    if (s_report_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(hid_task, "usb_ext_hid", 3072, NULL, 5, &s_hid_task) != pdPASS) {
        vQueueDelete(s_report_queue);
        s_report_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void usb_extend_hid_send_touch(const esp_lcd_touch_point_data_t *points, uint8_t count)
{
    if (s_report_queue == NULL || points == NULL || !usb_extend_screen_active_internal()) {
        return;
    }

    usb_extend_touch_report_t report = {0};
    report.count = count > USB_EXTEND_TOUCH_POINTS ? USB_EXTEND_TOUCH_POINTS : count;
    for (uint8_t i = 0; i < report.count; ++i) {
        uint16_t raw_x = clamp_coordinate(points[i].x, USB_EXTEND_SCREEN_HEIGHT);
        uint16_t raw_y = clamp_coordinate(points[i].y, USB_EXTEND_SCREEN_WIDTH);
        report.data[i].press_down = 1;
        report.data[i].index = points[i].track_id;
        report.data[i].x = USB_EXTEND_SCREEN_WIDTH - raw_y - 1;
        report.data[i].y = raw_x;
        report.data[i].width = points[i].strength;
        report.data[i].height = points[i].strength;
    }
    xQueueOverwrite(s_report_queue, &report);
}

void usb_extend_hid_send_release(void)
{
    if (s_report_queue == NULL) {
        return;
    }
    usb_extend_touch_report_t report = {0};
    xQueueOverwrite(s_report_queue, &report);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t requested_length)
{
    (void)instance;
    (void)report_type;
    if (report_id == USB_EXTEND_REPORT_ID_MAX_COUNT && requested_length > 0) {
        buffer[0] = USB_EXTEND_TOUCH_POINTS;
        return 1;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, const uint8_t *buffer,
                           uint16_t buffer_size)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)buffer_size;
}
