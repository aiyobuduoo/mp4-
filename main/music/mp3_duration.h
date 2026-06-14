#ifndef MP3_DURATION_H
#define MP3_DURATION_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    MP3_DURATION_SOURCE_NONE = 0,
    MP3_DURATION_SOURCE_XING,
    MP3_DURATION_SOURCE_VBRI,
    MP3_DURATION_SOURCE_CBR,
} mp3_duration_source_t;

typedef struct {
    uint32_t duration_ms;
    uint32_t sample_rate_hz;
    uint32_t bitrate_bps;
    uint32_t audio_frame_count;
    mp3_duration_source_t source;
} mp3_duration_info_t;

esp_err_t mp3_duration_read(const char *path, mp3_duration_info_t *info);
const char *mp3_duration_source_name(mp3_duration_source_t source);

#endif
