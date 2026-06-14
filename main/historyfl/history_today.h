#ifndef APP_HISTORY_TODAY_H
#define APP_HISTORY_TODAY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_TODAY_TEXT_MAX_LEN 512

esp_err_t history_today_task_start(void);
bool history_today_get_latest(char *out_text, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
