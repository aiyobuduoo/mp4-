#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "sd.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_log.h"
#if APP_SDMMC_USE_ONCHIP_LDO
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#define TAG "SDCARD"

static bool s_sd_mounted = false;
static sdmmc_card_t *s_card = NULL;
static esp_err_t s_last_sd_err = ESP_OK;
static char s_last_debug[192] = "sd idle";
static char s_debug_dump[1024] = "sd init log:\n";

static void debug_dump_reset(void)
{
    snprintf(s_debug_dump, sizeof(s_debug_dump), "sd init log:\n");
}

static void debug_dump_append(const char *line)
{
    size_t used = strlen(s_debug_dump);
    if (used >= sizeof(s_debug_dump) - 2) {
        return;
    }
    snprintf(s_debug_dump + used, sizeof(s_debug_dump) - used, "%s\n", line);
}

static void update_last_status(esp_err_t err, const char *msg)
{
    s_last_sd_err = err;
    snprintf(s_last_debug, sizeof(s_last_debug), "%s", msg ? msg : "sd status unknown");
    debug_dump_append(s_last_debug);
}

static void fill_slot_pins(sdmmc_slot_config_t *slot_config, int width)
{
    slot_config->clk = BSP_SD_CLK;
    slot_config->cmd = BSP_SD_CMD;
    slot_config->d0 = BSP_SD_D0;

    if (width >= 4) {
        slot_config->d1 = BSP_SD_D1;
        slot_config->d2 = BSP_SD_D2;
        slot_config->d3 = BSP_SD_D3;
    } else {
        slot_config->d1 = GPIO_NUM_NC;
        slot_config->d2 = GPIO_NUM_NC;
        slot_config->d3 = GPIO_NUM_NC;
    }
}

static void enable_sd_power_if_needed(void)
{
    int pin = (int)APP_SDMMC_POWER_EN_PIN;
    if (pin < 0 || pin >= 64) {
        return;
    }

    uint64_t mask = (1ULL << pin);
    gpio_config_t io_cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level((gpio_num_t)pin, APP_SDMMC_POWER_EN_LEVEL ? 1 : 0);
    if (APP_SDMMC_POWER_ON_DELAY_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(APP_SDMMC_POWER_ON_DELAY_MS));
    }
}

static void enable_onchip_ldo_if_needed(sdmmc_host_t *host)
{
#if APP_SDMMC_USE_ONCHIP_LDO
    if (!host) {
        return;
    }
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = APP_SDMMC_LDO_CHAN_ID,
    };
    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr);
    if (err == ESP_OK && pwr) {
        host->pwr_ctrl_handle = pwr;
        ESP_LOGI(TAG, "on-chip LDO enabled for SD: chan=%d", APP_SDMMC_LDO_CHAN_ID);
    } else {
        ESP_LOGW(TAG, "on-chip LDO enable failed: %s", esp_err_to_name(err));
    }
#else
    (void)host;
#endif
}

static int safe_gpio_level(gpio_num_t pin)
{
    if ((int)pin < 0 || (int)pin >= 64) {
        return -1;
    }
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << (int)pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    return gpio_get_level(pin);
}

static void log_sd_hw_diag(void)
{
    ESP_LOGI(TAG,
             "cfg: slot=%d width=%d freq=%dk pwr_pin=%d pwr_level=%d pwr_delay=%dms cd_pin=%d cd_active=%d",
             APP_SDMMC_SLOT, APP_SDMMC_WIDTH, APP_SDMMC_FREQ_KHZ,
             (int)APP_SDMMC_POWER_EN_PIN, APP_SDMMC_POWER_EN_LEVEL, APP_SDMMC_POWER_ON_DELAY_MS,
             (int)APP_SDMMC_CD_PIN, APP_SDMMC_CD_ACTIVE_LEVEL);

    int clk = safe_gpio_level(BSP_SD_CLK);
    int cmd = safe_gpio_level(BSP_SD_CMD);
    int d0 = safe_gpio_level(BSP_SD_D0);
    ESP_LOGI(TAG, "idle level: CLK=%d CMD=%d D0=%d", clk, cmd, d0);

    if ((int)APP_SDMMC_CD_PIN >= 0 && (int)APP_SDMMC_CD_PIN < 64) {
        int cd = safe_gpio_level(APP_SDMMC_CD_PIN);
        bool inserted = (cd == APP_SDMMC_CD_ACTIVE_LEVEL);
        ESP_LOGI(TAG, "card-detect: level=%d inserted=%s", cd, inserted ? "yes" : "no");
    } else {
        ESP_LOGI(TAG, "card-detect: disabled");
    }
}

esp_err_t sdcard_driver_mount(void)
{
    if (s_sd_mounted) {
        update_last_status(ESP_OK, "already mounted");
        return ESP_OK;
    }

    debug_dump_reset();

    const int slot_try[] = { APP_SDMMC_SLOT, 0, 1 };
    const int width_try[2] = { APP_SDMMC_WIDTH, 1 };
    const int freq_try[] = { APP_SDMMC_FREQ_KHZ, 30000, 20000, 10000 };
    const int min_accept_freq_khz = 10000;

    enable_sd_power_if_needed();
    log_sd_hw_diag();

    for (size_t si = 0; si < sizeof(slot_try) / sizeof(slot_try[0]); si++) {
        int slot = slot_try[si];
        if ((slot != 0 && slot != 1) || (si > 0 && slot == slot_try[0])) {
            continue;
        }

        for (size_t wi = 0; wi < sizeof(width_try) / sizeof(width_try[0]); wi++) {
        int width = width_try[wi];
        if (width != 1 && width != 4) {
            width = 1;
        }
        if (wi > 0 && width == width_try[0]) {
            continue;
        }

        for (size_t fi = 0; fi < sizeof(freq_try) / sizeof(freq_try[0]); fi++) {
            if (fi > 0 && freq_try[fi] == freq_try[0]) {
                continue;
            }
            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.slot = slot;
            host.max_freq_khz = freq_try[fi];
            enable_onchip_ldo_if_needed(&host);

            sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
            slot_config.width = width;
            slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
            fill_slot_pins(&slot_config, width);

            esp_vfs_fat_sdmmc_mount_config_t mount_config = {
                .format_if_mount_failed = false,
                .max_files = 8,
                .allocation_unit_size = 32 * 1024,
            };

            sdmmc_card_t *card = NULL;
            esp_err_t err = esp_vfs_fat_sdmmc_mount(
                APP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);

            if (err == ESP_OK && card != NULL) {
                if (freq_try[fi] < min_accept_freq_khz) {
                    char line_low[160];
                    snprintf(line_low, sizeof(line_low),
                             "mount reject low-speed slot=%d w=%d f=%dk (<%dk)",
                             slot, width, freq_try[fi], min_accept_freq_khz);
                    update_last_status(ESP_ERR_INVALID_STATE, line_low);
                    ESP_LOGW(TAG, "%s", line_low);
                    (void)esp_vfs_fat_sdcard_unmount(APP_SD_MOUNT_POINT, card);
                    continue;
                }
                s_card = card;
                s_sd_mounted = true;
                sdmmc_card_print_info(stdout, card);

                char line[128];
                snprintf(line, sizeof(line), "mount ok slot=%d w=%d f=%dk", slot, width, freq_try[fi]);
                update_last_status(ESP_OK, line);
                ESP_LOGI(TAG, "SD mounted: slot=%d width=%d freq=%dKHz", slot, width, freq_try[fi]);
                return ESP_OK;
            }

            char line[160];
            snprintf(line, sizeof(line), "mount fail slot=%d w=%d f=%dk err=%s",
                     slot, width, freq_try[fi], esp_err_to_name(err));
            update_last_status(err, line);
            ESP_LOGW(TAG, "%s", line);
        }
    }
    }

    update_last_status(s_last_sd_err, "mount failed all retries (high-speed only)");
    ESP_LOGE(TAG, "All SD mount retries failed");
    return s_last_sd_err;
}

esp_err_t sdcard_driver_unmount(void)
{
    if (!s_sd_mounted) {
        update_last_status(ESP_OK, "unmount skipped: not mounted");
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(APP_SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_sd_mounted = false;
        s_card = NULL;
        update_last_status(ESP_OK, "unmount ok");
        return ESP_OK;
    }

    char line[128];
    snprintf(line, sizeof(line), "unmount fail err=%s", esp_err_to_name(err));
    update_last_status(err, line);
    return err;
}

bool sdcard_driver_is_mounted(void)
{
    return s_sd_mounted;
}

sdmmc_card_t *sdcard_driver_get_card(void)
{
    return s_card;
}

void sdcard_init(void)
{
    (void)sdcard_driver_mount();
}

bool sdcard_try_init(void)
{
    return sdcard_driver_mount() == ESP_OK;
}

bool sdcard_is_mounted(void)
{
    return sdcard_driver_is_mounted();
}

esp_err_t sdcard_last_error(void)
{
    return s_last_sd_err;
}

const char *sdcard_last_debug(void)
{
    return s_last_debug;
}

const char *sdcard_debug_dump(void)
{
    return s_debug_dump;
}

void list_sdcard_files(const char *path)
{
    if (!s_sd_mounted) {
        update_last_status(ESP_ERR_INVALID_STATE, "list fail: sd not mounted");
        ESP_LOGE(TAG, "list fail: sd not mounted");
        return;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        char line[160];
        snprintf(line, sizeof(line), "open dir fail: %s", path ? path : "<null>");
        update_last_status(ESP_FAIL, line);
        ESP_LOGE(TAG, "%s", line);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            ESP_LOGI(TAG, "DIR : %s", entry->d_name);
        } else if (entry->d_type == DT_REG) {
            ESP_LOGI(TAG, "FILE: %s", entry->d_name);
        } else {
            ESP_LOGI(TAG, "OTHER: %s", entry->d_name);
        }
    }

    closedir(dir);
    update_last_status(ESP_OK, "list ok");
}

int sdcard_read_text_file(const char *path, char *out, size_t out_size)
{
    if (!s_sd_mounted) {
        update_last_status(ESP_ERR_INVALID_STATE, "read fail: sd not mounted");
        return -1;
    }

    if (!path || !out || out_size < 2) {
        update_last_status(ESP_ERR_INVALID_ARG, "read arg invalid");
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char line[192];
        snprintf(line, sizeof(line), "read open fail: %s", path);
        update_last_status(ESP_FAIL, line);
        return -1;
    }

    size_t n = fread(out, 1, out_size - 1, fp);
    out[n] = '\0';
    fclose(fp);

    char line[192];
    snprintf(line, sizeof(line), "read ok: %s (%uB)", path, (unsigned)n);
    update_last_status(ESP_OK, line);
    return (int)n;
}
