#ifndef MP3_COVER_H
#define MP3_COVER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t *data; /* Packed RGB565 pixels, optionally followed by alpha per pixel. */
    size_t size;
    uint16_t width;
    uint16_t height;
    bool has_alpha;
} mp3_cover_t;

esp_err_t mp3_cover_read(const char *path, mp3_cover_t *cover);
void mp3_cover_free(mp3_cover_t *cover);

#endif
