#include "usb_extend_screen_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define USB_EXTEND_FRAME_COUNT 3

static QueueHandle_t s_empty_queue;
static QueueHandle_t s_filled_queue;

static void free_frame(usb_extend_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    heap_caps_free(frame->data);
    free(frame);
}

static void frame_pool_cleanup(void)
{
    usb_extend_frame_t *frame = NULL;
    if (s_empty_queue != NULL) {
        while (xQueueReceive(s_empty_queue, &frame, 0) == pdPASS) {
            free_frame(frame);
        }
        vQueueDelete(s_empty_queue);
        s_empty_queue = NULL;
    }
    if (s_filled_queue != NULL) {
        while (xQueueReceive(s_filled_queue, &frame, 0) == pdPASS) {
            free_frame(frame);
        }
        vQueueDelete(s_filled_queue);
        s_filled_queue = NULL;
    }
}

esp_err_t usb_extend_frame_pool_init(void)
{
    if (s_empty_queue != NULL) {
        return ESP_OK;
    }

    s_empty_queue = xQueueCreate(USB_EXTEND_FRAME_COUNT, sizeof(usb_extend_frame_t *));
    s_filled_queue = xQueueCreate(USB_EXTEND_FRAME_COUNT, sizeof(usb_extend_frame_t *));
    if (s_empty_queue == NULL || s_filled_queue == NULL) {
        frame_pool_cleanup();
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < USB_EXTEND_FRAME_COUNT; ++i) {
        usb_extend_frame_t *frame = calloc(1, sizeof(*frame));
        if (frame == NULL) {
            frame_pool_cleanup();
            return ESP_ERR_NO_MEM;
        }
        frame->data = heap_caps_aligned_alloc(16, USB_EXTEND_SCREEN_FRAME_LIMIT,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (frame->data == NULL) {
            free(frame);
            frame_pool_cleanup();
            return ESP_ERR_NO_MEM;
        }
        frame->capacity = USB_EXTEND_SCREEN_FRAME_LIMIT;
        if (xQueueSend(s_empty_queue, &frame, 0) != pdPASS) {
            free_frame(frame);
            frame_pool_cleanup();
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

usb_extend_frame_t *usb_extend_frame_get_empty(void)
{
    usb_extend_frame_t *frame = NULL;
    xQueueReceive(s_empty_queue, &frame, 0);
    return frame;
}

usb_extend_frame_t *usb_extend_frame_get_filled(void)
{
    usb_extend_frame_t *frame = NULL;
    xQueueReceive(s_filled_queue, &frame, portMAX_DELAY);
    return frame;
}

esp_err_t usb_extend_frame_append(usb_extend_frame_t *frame, const uint8_t *data, size_t length)
{
    if (frame == NULL || data == NULL || length == 0 || frame->length + length > frame->capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(frame->data + frame->length, data, length);
    frame->length += length;
    frame->info.received += length;
    return ESP_OK;
}

esp_err_t usb_extend_frame_send_filled(usb_extend_frame_t *frame)
{
    return xQueueSend(s_filled_queue, &frame, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t usb_extend_frame_return_empty(usb_extend_frame_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    frame->length = 0;
    memset(&frame->info, 0, sizeof(frame->info));
    return xQueueSend(s_empty_queue, &frame, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void usb_extend_frame_drain_filled(void)
{
    if (s_filled_queue == NULL) {
        return;
    }

    usb_extend_frame_t *frame = NULL;
    while (xQueueReceive(s_filled_queue, &frame, 0) == pdPASS) {
        usb_extend_frame_return_empty(frame);
    }
}
