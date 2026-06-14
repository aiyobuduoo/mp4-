#include "es8311_code.h"

#include <stdbool.h>
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "esp_log.h"

#define ES8311_I2C_TIMEOUT_MS      1000
#define ES8311_I2S_PORT_NUM        I2S_NUM_0
#define ES8311_I2S_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define ES8311_I2S_CHANNELS        2
#define ES8311_I2S_CHANNEL_MASK    0x03
/*
 * config.h keeps the ES8311 slave address in 7-bit form for ESP-IDF I2C APIs.
 * esp_codec_dev's audio_codec_i2c_cfg_t.addr uses the 8-bit device address form.
 */
#define ES8311_CODEC_CTRL_ADDR     (AUDIO_CODEC_ES8311_ADDR << 1)

static const char *TAG = "es8311_codec";

static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static i2c_master_bus_handle_t s_i2c_bus_handle;
static esp_codec_dev_handle_t s_codec_handle;
static bool s_codec_inited;
static uint8_t s_output_volume = APP_OUTPUT_VOLUME;
static uint8_t s_input_gain = APP_INPUT_GAIN;
static esp_codec_dev_sample_info_t s_sample_cfg = {
    .bits_per_sample = ES8311_I2S_BITS_PER_SAMPLE,
    .channel = ES8311_I2S_CHANNELS,
    .channel_mask = ES8311_I2S_CHANNEL_MASK,
    .sample_rate = AUDIO_OUTPUT_SAMPLE_RATE,
};

static esp_err_t es8311_i2s_init(void)
{
    if (s_tx_handle && s_rx_handle) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(ES8311_I2S_PORT_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "create i2s channels failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_OUTPUT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(ES8311_I2S_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = AUDIO_I2S_GPIO_DOUT,
            .din = AUDIO_I2S_GPIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "init i2s tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "init i2s rx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable i2s tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx failed");

    return ESP_OK;
}

static esp_err_t es8311_i2c_init(void)
{
    if (s_i2c_bus_handle) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus_handle), TAG, "create i2c bus failed");
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c_bus_handle, AUDIO_CODEC_ES8311_ADDR, ES8311_I2C_TIMEOUT_MS),
                        TAG, "probe es8311 addr 0x%02x failed", AUDIO_CODEC_ES8311_ADDR);

    return ESP_OK;
}

esp_err_t es8311_codec_init(void)
{
    if (s_codec_inited) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(es8311_i2s_init(), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(es8311_i2c_init(), TAG, "i2c init failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_CTRL_ADDR,
        .bus_handle = s_i2c_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec ctrl_if failed");

    audio_codec_i2s_cfg_t data_cfg = {
        .port = ES8311_I2S_PORT_NUM,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec data_if failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec gpio_if failed");

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = AUDIO_I2S_GPIO_MCLK != GPIO_NUM_NC,
        .pa_pin = AUDIO_CODEC_PA_PIN,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create es8311 interface failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec_handle = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec_handle != NULL, ESP_ERR_NO_MEM, TAG, "create codec device failed");

    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec_handle, &s_sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec_handle, s_output_volume) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set output volume failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec_handle, s_input_gain) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set input gain failed");

    s_codec_inited = true;
    ESP_LOGI(TAG,
             "ES8311 ready: i2c7=0x%02x i2c8=0x%02x mclk=%d bclk=%d ws=%d dout=%d din=%d sample_rate=%d",
             AUDIO_CODEC_ES8311_ADDR,
             ES8311_CODEC_CTRL_ADDR,
             AUDIO_I2S_GPIO_MCLK,
             AUDIO_I2S_GPIO_BCLK,
             AUDIO_I2S_GPIO_WS,
             AUDIO_I2S_GPIO_DOUT,
             AUDIO_I2S_GPIO_DIN,
             s_sample_cfg.sample_rate);

    return ESP_OK;
}

esp_err_t es8311_codec_set_stream_format(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample)
{
    ESP_RETURN_ON_FALSE(s_codec_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "codec not initialized");
    ESP_RETURN_ON_FALSE(sample_rate > 0, ESP_ERR_INVALID_ARG, TAG, "invalid sample rate");
    ESP_RETURN_ON_FALSE(channels == 1 || channels == 2, ESP_ERR_INVALID_ARG, TAG, "invalid channel count");

    uint16_t channel_mask = (channels == 1) ? 0x01 : 0x03;
    if (s_sample_cfg.sample_rate == sample_rate &&
        s_sample_cfg.channel == channels &&
        s_sample_cfg.bits_per_sample == bits_per_sample &&
        s_sample_cfg.channel_mask == channel_mask) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Reconfiguring ES8311 stream: %lu Hz, %u ch, %u bits",
             (unsigned long)sample_rate, channels, bits_per_sample);

    if (esp_codec_dev_close(s_codec_handle) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec close before reconfig failed");
    }

    s_sample_cfg.sample_rate = sample_rate;
    s_sample_cfg.channel = channels;
    s_sample_cfg.bits_per_sample = bits_per_sample;
    s_sample_cfg.channel_mask = channel_mask;

    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec_handle, &s_sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "re-open codec failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec_handle, s_output_volume) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "restore output volume failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec_handle, s_input_gain) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "restore input gain failed");
    return ESP_OK;
}

esp_err_t es8311_codec_deinit(void)
{
    if (s_tx_handle) {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    if (s_rx_handle) {
        i2s_channel_disable(s_rx_handle);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
    if (s_i2c_bus_handle) {
        i2c_del_master_bus(s_i2c_bus_handle);
        s_i2c_bus_handle = NULL;
    }

    s_codec_handle = NULL;
    s_codec_inited = false;
    return ESP_OK;
}

esp_err_t es8311_codec_set_volume(uint8_t volume)
{
    ESP_RETURN_ON_FALSE(s_codec_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "codec not initialized");
    s_output_volume = volume;
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec_handle, volume) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set volume failed");
    return ESP_OK;
}

esp_err_t es8311_codec_set_input_gain(uint8_t gain)
{
    ESP_RETURN_ON_FALSE(s_codec_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "codec not initialized");
    s_input_gain = gain;
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec_handle, gain) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set input gain failed");
    return ESP_OK;
}

esp_err_t es8311_codec_write(const void *data, size_t size, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)timeout_ms;
    ESP_RETURN_ON_FALSE(s_codec_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "codec not initialized");
    int ret = esp_codec_dev_write(s_codec_handle, (void *)data, (int)size);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (bytes_written) {
        *bytes_written = size;
    }
    return ESP_OK;
}

esp_err_t es8311_codec_read(void *data, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_rx_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "rx not initialized");
    return i2s_channel_read(s_rx_handle, data, size, bytes_read, timeout_ms);
}
