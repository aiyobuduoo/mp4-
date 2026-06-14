#ifndef MJPEG_PLAYER_H
#define MJPEG_PLAYER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t mjpeg_player_start(const char *base_url);
esp_err_t mjpeg_player_switch_relative(int delta);
void mjpeg_player_set_active(bool active);
bool mjpeg_player_is_active(void);
void mjpeg_player_stop_for_ws(void);

#endif
