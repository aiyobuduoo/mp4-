#ifndef SD_H_
#define SD_H_

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SDCARD_MOUNT_POINT APP_SD_MOUNT_POINT

/* Driver style API */
esp_err_t sdcard_driver_mount(void);
esp_err_t sdcard_driver_unmount(void);
bool sdcard_driver_is_mounted(void);
sdmmc_card_t *sdcard_driver_get_card(void);

/* Compatibility API used by existing app code */
void sdcard_init(void);
bool sdcard_try_init(void);
bool sdcard_is_mounted(void);
esp_err_t sdcard_last_error(void);
const char *sdcard_last_debug(void);
const char *sdcard_debug_dump(void);
void list_sdcard_files(const char *path);
int sdcard_read_text_file(const char *path, char *out, size_t out_size);

#endif
