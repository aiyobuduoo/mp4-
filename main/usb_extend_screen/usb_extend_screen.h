#ifndef APP_USB_EXTEND_SCREEN_H
#define APP_USB_EXTEND_SCREEN_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_extend_screen_start(void);
esp_err_t usb_extend_screen_stop(void);
bool usb_extend_screen_is_active(void);

#ifdef __cplusplus
}
#endif

#endif
