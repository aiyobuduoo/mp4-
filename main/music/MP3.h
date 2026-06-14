#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mp3_init(void);
void mp3_play_url(const char *url);
void mp3_play_file(const char *path);
void mp3_play_file_from_offset(const char *path, size_t offset, uint32_t position_ms);
void mp3_play_first_file_in_dir(const char *dir_path);
void mp3_stop(void);
bool mp3_stop_and_wait(uint32_t timeout_ms);
uint32_t mp3_get_position_ms(void);
bool mp3_is_playing(void);

#ifdef __cplusplus
}
#endif
