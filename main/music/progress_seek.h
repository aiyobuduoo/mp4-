#ifndef PROGRESS_SEEK_H
#define PROGRESS_SEEK_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl/lvgl.h"

typedef void (*progress_seek_commit_cb_t)(uint32_t target_ms, void *user_data);

void progress_seek_init(lv_obj_t *slider, lv_obj_t *time_label,
                        progress_seek_commit_cb_t commit_cb, void *user_data);
void progress_seek_set_duration(uint32_t duration_ms);
void progress_seek_update(uint32_t position_ms);
bool progress_seek_is_dragging(void);
esp_err_t progress_seek_find_mp3_frame(const char *path, uint32_t duration_ms,
                                       uint32_t target_ms, size_t *frame_offset);

#endif
