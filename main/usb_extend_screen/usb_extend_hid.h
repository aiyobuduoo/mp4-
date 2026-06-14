#ifndef APP_USB_EXTEND_HID_H
#define APP_USB_EXTEND_HID_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "usb_extend_hid_descriptor.h"

typedef struct {
    uint8_t press_down;
    uint8_t index;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} __attribute__((packed)) usb_extend_touch_point_report_t;

typedef struct {
    usb_extend_touch_point_report_t data[USB_EXTEND_TOUCH_POINTS];
    uint8_t count;
} __attribute__((packed)) usb_extend_touch_report_t;

esp_err_t usb_extend_hid_init(void);
void usb_extend_hid_send_touch(const esp_lcd_touch_point_data_t *points, uint8_t count);
void usb_extend_hid_send_release(void);

#endif
