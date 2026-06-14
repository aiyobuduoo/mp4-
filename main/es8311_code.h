#ifndef ES8311_CODE_H
#define ES8311_CODE_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t es8311_codec_init(void);
esp_err_t es8311_codec_deinit(void);
esp_err_t es8311_codec_set_stream_format(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
esp_err_t es8311_codec_set_volume(uint8_t volume);
esp_err_t es8311_codec_set_input_gain(uint8_t gain);
esp_err_t es8311_codec_write(const void *data, size_t size, size_t *bytes_written, uint32_t timeout_ms);
esp_err_t es8311_codec_read(void *data, size_t size, size_t *bytes_read, uint32_t timeout_ms);

#endif
