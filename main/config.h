#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <driver/gpio.h>
#include "driver/i2c_master.h"

/* Debug switch: set 1 to skip Wi-Fi init in app_main for SD bring-up */
#define APP_SKIP_WIFI_INIT    0
/* Debug switch: set 1 to skip SD init in app_main */
#define APP_SKIP_SD_INIT      0

#define APP_OUTPUT_VOLUME     60
#define APP_INPUT_GAIN        30

#define APP_FIRMWARE_VERSION        "1.0.2"
#define APP_OTA_LATEST_URL          "http://192.168.123.169:5001/latest.json"
#define APP_OTA_CHECK_INTERVAL_MS   (10 * 60 * 1000)
#define APP_MJPEG_SD_DIR            APP_SD_MOUNT_POINT "/MJPEG"
#define APP_MJPEG_TARGET_FPS        30
#define APP_MJPEG_CACHE_MIN_BYTES   (10 * 1024 * 1024)
#define APP_MJPEG_CACHE_MAX_BYTES   (10 * 1024 * 1024)
#define APP_MJPEG_CACHE_RESERVE_BYTES (512 * 1024)
#define APP_MJPEG_PREFILL_BYTES     (256 * 1024)
#define APP_MJPEG_HTTP_READ_CHUNK   4096
#define APP_MJPEG_HTTP_TIMEOUT_MS   5000

/* Audio / pin config */
#define APP_AUDIO_INPUT_SAMPLE_RATE   16000
#define APP_AUDIO_OUTPUT_SAMPLE_RATE  16000
#define APP_AUDIO_MUSIC_SAMPLE_RATE   48000

#define APP_AUDIO_I2S_GPIO_MCLK       GPIO_NUM_13
#define APP_AUDIO_I2S_GPIO_WS         GPIO_NUM_10
#define APP_AUDIO_I2S_GPIO_BCLK       GPIO_NUM_12
#define APP_AUDIO_I2S_GPIO_DIN        GPIO_NUM_48
#define APP_AUDIO_I2S_GPIO_DOUT       GPIO_NUM_9

#define APP_AUDIO_CODEC_PA_PIN        GPIO_NUM_11
#define APP_AUDIO_CODEC_I2C_SDA_PIN   GPIO_NUM_7
#define APP_AUDIO_CODEC_I2C_SCL_PIN   GPIO_NUM_8
#define APP_AUDIO_CODEC_ES8311_ADDR   0x18

/* LCD / touch / backlight config */
#define APP_LCD_H_RES                 480
#define APP_LCD_V_RES                 800
#define APP_LCD_DPI_BUFFER_NUMS       1

#define APP_LCD_I2C_NUM               I2C_NUM_1
#define APP_LCD_I2C_SDA               GPIO_NUM_7
#define APP_LCD_I2C_SCL               GPIO_NUM_8

#define APP_LCD_TOUCH_RST             GPIO_NUM_NC
#define APP_LCD_TOUCH_INT             GPIO_NUM_NC
#define APP_LCD_TOUCH_ENABLE          1
#define APP_LCD_TOUCH_I2C_ADDR        0x5D
#define APP_LCD_TOUCH_SWAP_XY         0
#define APP_LCD_TOUCH_MIRROR_X        0
#define APP_LCD_TOUCH_MIRROR_Y        0
#define APP_LCD_RST                   GPIO_NUM_5

#define APP_LCD_BACKLIGHT_PIN         GPIO_NUM_23
#define APP_SPLASH_FRAME_SETTLE_MS    100

#define APP_SD_MOUNT_POINT "/sdcard"

/*
 * SDMMC host slot and bus settings.
 * Keep width=1 first for bring-up stability, then switch to 4-bit if wiring supports it.
 */
#define APP_SDMMC_SLOT 0
#define APP_SDMMC_WIDTH 4
#define APP_SDMMC_FREQ_KHZ 40000

/*
 * External SD card power control pin (optional).
 * Set to GPIO_NUM_NC if your board powers SD card directly.
 */
#define APP_SDMMC_POWER_EN_PIN GPIO_NUM_NC
#define APP_SDMMC_POWER_EN_LEVEL 1
#define APP_SDMMC_POWER_ON_DELAY_MS 100

/*
 * Optional card-detect pin (active-low on many sockets).
 * Set GPIO_NUM_NC to disable CD checking.
 */
#define APP_SDMMC_CD_PIN GPIO_NUM_NC
#define APP_SDMMC_CD_ACTIVE_LEVEL 0

/*
 * On-chip SD LDO control (optional, ESP32-P4 only when hardware requires it).
 */
#ifndef APP_SDMMC_USE_ONCHIP_LDO
#define APP_SDMMC_USE_ONCHIP_LDO 1
#endif

#ifndef APP_SDMMC_LDO_CHAN_ID
#define APP_SDMMC_LDO_CHAN_ID 4
#endif

#ifndef APP_SDMMC_IO_VOLTAGE
#define APP_SDMMC_IO_VOLTAGE 3.3f
#endif

/*
 * Default SDMMC pin map for ESP32-P4 EV-style wiring.
 * If your board uses different pins, change these 6 macros only.
 */
#define BSP_SD_CLK GPIO_NUM_43
#define BSP_SD_CMD GPIO_NUM_44
#define BSP_SD_D0  GPIO_NUM_39
#define BSP_SD_D1  GPIO_NUM_40
#define BSP_SD_D2  GPIO_NUM_41
#define BSP_SD_D3  GPIO_NUM_42

/* Backward-compatible aliases */
#define AUDIO_INPUT_SAMPLE_RATE       APP_AUDIO_INPUT_SAMPLE_RATE
#define AUDIO_OUTPUT_SAMPLE_RATE      APP_AUDIO_OUTPUT_SAMPLE_RATE
#define AUDIO_I2S_GPIO_MCLK           APP_AUDIO_I2S_GPIO_MCLK
#define AUDIO_I2S_GPIO_WS             APP_AUDIO_I2S_GPIO_WS
#define AUDIO_I2S_GPIO_BCLK           APP_AUDIO_I2S_GPIO_BCLK
#define AUDIO_I2S_GPIO_DIN            APP_AUDIO_I2S_GPIO_DIN
#define AUDIO_I2S_GPIO_DOUT           APP_AUDIO_I2S_GPIO_DOUT
#define AUDIO_CODEC_PA_PIN            APP_AUDIO_CODEC_PA_PIN
#define AUDIO_CODEC_I2C_SDA_PIN       APP_AUDIO_CODEC_I2C_SDA_PIN
#define AUDIO_CODEC_I2C_SCL_PIN       APP_AUDIO_CODEC_I2C_SCL_PIN
#define AUDIO_CODEC_ES8311_ADDR       APP_AUDIO_CODEC_ES8311_ADDR

#define BSP_LCD_H_RES                 APP_LCD_H_RES
#define BSP_LCD_V_RES                 APP_LCD_V_RES
#define BSP_LCD_DPI_BUFFER_NUMS       APP_LCD_DPI_BUFFER_NUMS
#define BSP_I2C_NUM                   APP_LCD_I2C_NUM
#define BSP_I2C_SDA                   APP_LCD_I2C_SDA
#define BSP_I2C_SCL                   APP_LCD_I2C_SCL
#define BSP_LCD_TOUCH_RST             APP_LCD_TOUCH_RST
#define BSP_LCD_TOUCH_INT             APP_LCD_TOUCH_INT
#define BSP_LCD_RST                   APP_LCD_RST
#define BSP_LCD_BACKLIGHT             APP_LCD_BACKLIGHT_PIN

#define SD_MOUNT_POINT                APP_SD_MOUNT_POINT

#endif


