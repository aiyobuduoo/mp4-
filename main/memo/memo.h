#ifndef APP_MEMO_H
#define APP_MEMO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEMO_TEXT_MAX_LEN 1200

esp_err_t memo_service_start(void);
bool memo_get_latest(char *out_text, size_t out_size);
uint16_t memo_service_port(void);

#ifdef __cplusplus
}
#endif

#endif
