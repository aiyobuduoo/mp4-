#ifndef GECILRC_H
#define GECILRC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t gecilrc_init(void);
esp_err_t gecilrc_load_for_track(const char *title, const char *artist);
const char *gecilrc_get_line(uint32_t position_ms);
const char *gecilrc_get_matched_path(void);
size_t gecilrc_get_line_count(void);
bool gecilrc_is_loaded(void);

#endif
