#ifndef APP_USB_EXTEND_SCREEN_INTERNAL_H
#define APP_USB_EXTEND_SCREEN_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define USB_EXTEND_SCREEN_WIDTH       800
#define USB_EXTEND_SCREEN_HEIGHT      480
#define USB_EXTEND_SCREEN_FRAME_LIMIT (300 * 1024)

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t received;
    uint32_t total;
} usb_extend_frame_info_t;

typedef struct {
    size_t capacity;
    size_t length;
    uint8_t *data;
    usb_extend_frame_info_t info;
} usb_extend_frame_t;

esp_err_t usb_extend_frame_pool_init(void);
usb_extend_frame_t *usb_extend_frame_get_empty(void);
usb_extend_frame_t *usb_extend_frame_get_filled(void);
esp_err_t usb_extend_frame_append(usb_extend_frame_t *frame, const uint8_t *data, size_t length);
esp_err_t usb_extend_frame_send_filled(usb_extend_frame_t *frame);
esp_err_t usb_extend_frame_return_empty(usb_extend_frame_t *frame);
void usb_extend_frame_drain_filled(void);

esp_err_t usb_extend_display_init(void);
esp_err_t usb_extend_display_draw(const uint8_t *jpeg, size_t length);
void usb_extend_display_wait_idle(void);
esp_err_t usb_extend_vendor_init(void);
void usb_extend_vendor_reset(void);
esp_err_t usb_extend_hid_init(void);
esp_err_t usb_extend_usb_init(void);
void usb_extend_usb_connect(void);
void usb_extend_usb_disconnect(void);
bool usb_extend_screen_active_internal(void);

#endif
