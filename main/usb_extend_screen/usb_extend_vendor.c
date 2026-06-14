#include "usb_extend_screen_internal.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#define USB_EXTEND_VENDOR_RX_SIZE 512
#define USB_FRAME_TYPE_JPEG       3

typedef struct {
    uint16_t crc16;
    uint8_t type;
    uint8_t command;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id : 10;
    uint32_t payload_total : 22;
} __attribute__((packed)) usb_display_frame_header_t;

static const char *TAG = "usb_extend_vendor";
static TaskHandle_t s_transfer_task;
static usb_extend_frame_t *s_current_frame;
static bool s_skip_frame;
static uint32_t s_skip_total;
static uint32_t s_skip_received;

static void start_skip(uint32_t total, uint32_t received)
{
    s_skip_frame = received < total;
    s_skip_total = total;
    s_skip_received = received;
}

static void consume_skip(uint32_t length)
{
    s_skip_received += length;
    if (s_skip_received >= s_skip_total) {
        s_skip_frame = false;
    }
}

static void append_frame_data(const uint8_t *data, size_t length)
{
    if (s_current_frame == NULL || length == 0) {
        return;
    }
    if (usb_extend_frame_append(s_current_frame, data, length) != ESP_OK) {
        usb_extend_frame_return_empty(s_current_frame);
        s_current_frame = NULL;
        return;
    }
    if (s_current_frame->info.received >= s_current_frame->info.total) {
        if (usb_extend_frame_send_filled(s_current_frame) != ESP_OK) {
            usb_extend_frame_return_empty(s_current_frame);
        }
        s_current_frame = NULL;
    }
}

void tud_vendor_rx_cb(uint8_t interface, uint8_t const *buffer, uint16_t buffer_size)
{
    (void)buffer;
    (void)buffer_size;
    static uint8_t rx_buffer[USB_EXTEND_VENDOR_RX_SIZE];

    while (tud_vendor_n_available(interface) > 0) {
        if (s_current_frame == NULL && !s_skip_frame &&
            tud_vendor_n_available(interface) < sizeof(usb_display_frame_header_t)) {
            break;
        }

        int received = tud_vendor_n_read(interface, rx_buffer, sizeof(rx_buffer));
        if (received <= 0) {
            break;
        }
        if (!usb_extend_screen_active_internal()) {
            continue;
        }
        if (s_skip_frame) {
            consume_skip((uint32_t)received);
            continue;
        }
        if (s_current_frame != NULL) {
            append_frame_data(rx_buffer, (size_t)received);
            continue;
        }
        if ((size_t)received < sizeof(usb_display_frame_header_t)) {
            continue;
        }

        const usb_display_frame_header_t *header = (const usb_display_frame_header_t *)rx_buffer;
        uint32_t first_length = (uint32_t)received - sizeof(*header);
        if (header->type != USB_FRAME_TYPE_JPEG ||
            header->x != 0 || header->y != 0 ||
            header->width != USB_EXTEND_SCREEN_WIDTH ||
            header->height != USB_EXTEND_SCREEN_HEIGHT ||
            header->payload_total == 0 ||
            header->payload_total > USB_EXTEND_SCREEN_FRAME_LIMIT) {
            start_skip(header->payload_total, first_length);
            continue;
        }

        s_current_frame = usb_extend_frame_get_empty();
        if (s_current_frame == NULL) {
            start_skip(header->payload_total, first_length);
            continue;
        }
        s_current_frame->info.width = header->width;
        s_current_frame->info.height = header->height;
        s_current_frame->info.total = header->payload_total;
        append_frame_data(rx_buffer + sizeof(*header), first_length);
    }
}

static void transfer_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_extend_frame_t *frame = usb_extend_frame_get_filled();
        if (frame == NULL) {
            continue;
        }
        if (usb_extend_screen_active_internal()) {
            esp_err_t err = usb_extend_display_draw(frame->data, frame->length);
            if (err != ESP_OK) {
                ESP_LOGD(TAG, "Frame draw failed: %s", esp_err_to_name(err));
            }
        }
        usb_extend_frame_return_empty(frame);
    }
}

esp_err_t usb_extend_vendor_init(void)
{
    if (s_transfer_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(transfer_task, "usb_ext_draw", 4096, NULL, 5,
                                &s_transfer_task, 1) != pdPASS) {
        s_transfer_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void usb_extend_vendor_reset(void)
{
    if (s_current_frame != NULL) {
        usb_extend_frame_return_empty(s_current_frame);
        s_current_frame = NULL;
    }
    s_skip_frame = false;
    s_skip_total = 0;
    s_skip_received = 0;
    usb_extend_frame_drain_filled();
}
