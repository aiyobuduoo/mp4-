#include "mp3_cover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "lvgl/src/extra/libs/png/lodepng.h"

#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t read_syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static esp_err_t decode_jpeg_rgb565(const uint8_t *jpeg, size_t jpeg_size, mp3_cover_t *cover)
{
    jpeg_decode_picture_info_t info;
    esp_err_t err = jpeg_decoder_get_info(jpeg, jpeg_size, &info);
    if (err != ESP_OK || info.width > UINT16_MAX || info.height > UINT16_MAX) {
        return err == ESP_OK ? ESP_ERR_NOT_SUPPORTED : err;
    }

    uint32_t aligned_width = ALIGN_UP(info.width, 16);
    uint32_t aligned_height = ALIGN_UP(info.height, 16);
    size_t hardware_size = (size_t)aligned_width * aligned_height * 2;
    jpeg_decode_memory_alloc_cfg_t output_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    jpeg_decode_memory_alloc_cfg_t input_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t output_allocated = 0;
    uint8_t *hardware_pixels = jpeg_alloc_decoder_mem(hardware_size, &output_mem_cfg, &output_allocated);
    if (!hardware_pixels) {
        return ESP_ERR_NO_MEM;
    }

    size_t input_allocated = 0;
    uint8_t *hardware_jpeg = jpeg_alloc_decoder_mem(jpeg_size, &input_mem_cfg, &input_allocated);
    if (!hardware_jpeg) {
        free(hardware_pixels);
        return ESP_ERR_NO_MEM;
    }
    memcpy(hardware_jpeg, jpeg, jpeg_size);

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (err != ESP_OK) {
        free(hardware_jpeg);
        free(hardware_pixels);
        return err;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    err = jpeg_decoder_process(decoder, &decode_cfg, hardware_jpeg, jpeg_size,
                               hardware_pixels, output_allocated, &out_size);
    jpeg_del_decoder_engine(decoder);
    free(hardware_jpeg);
    if (err != ESP_OK) {
        free(hardware_pixels);
        return err;
    }

    size_t packed_size = (size_t)info.width * info.height * 2;
    uint8_t *packed = heap_caps_malloc(packed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packed) {
        packed = heap_caps_malloc(packed_size, MALLOC_CAP_8BIT);
    }
    if (!packed) {
        free(hardware_pixels);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t y = 0; y < info.height; y++) {
        memcpy(packed + (size_t)y * info.width * 2,
               hardware_pixels + (size_t)y * aligned_width * 2,
               (size_t)info.width * 2);
    }
    free(hardware_pixels);

    cover->data = packed;
    cover->size = packed_size;
    cover->width = info.width;
    cover->height = info.height;
    cover->has_alpha = false;
    return ESP_OK;
}

static esp_err_t decode_png_rgb565_alpha(const uint8_t *png, size_t png_size, mp3_cover_t *cover)
{
    unsigned char *rgba = NULL;
    unsigned width = 0;
    unsigned height = 0;
    unsigned decode_error = lodepng_decode32(&rgba, &width, &height, png, png_size);
    if (decode_error != 0 || width == 0 || height == 0 ||
        width > UINT16_MAX || height > UINT16_MAX) {
        free(rgba);
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t pixel_count = (size_t)width * height;
    if (pixel_count > SIZE_MAX / 3U) {
        free(rgba);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t packed_size = pixel_count * 3U;
    uint8_t *packed = heap_caps_malloc(packed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packed) {
        packed = heap_caps_malloc(packed_size, MALLOC_CAP_8BIT);
    }
    if (!packed) {
        free(rgba);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t red = rgba[i * 4U];
        uint8_t green = rgba[i * 4U + 1U];
        uint8_t blue = rgba[i * 4U + 2U];
        uint16_t rgb565 = ((uint16_t)(red & 0xf8U) << 8) |
                          ((uint16_t)(green & 0xfcU) << 3) |
                          (blue >> 3);
        packed[i * 3U] = (uint8_t)rgb565;
        packed[i * 3U + 1U] = (uint8_t)(rgb565 >> 8);
        packed[i * 3U + 2U] = rgba[i * 4U + 3U];
    }
    free(rgba);

    cover->data = packed;
    cover->size = packed_size;
    cover->width = (uint16_t)width;
    cover->height = (uint16_t)height;
    cover->has_alpha = true;
    return ESP_OK;
}

void mp3_cover_free(mp3_cover_t *cover)
{
    if (cover) {
        heap_caps_free(cover->data);
        memset(cover, 0, sizeof(*cover));
    }
}

esp_err_t mp3_cover_read(const char *path, mp3_cover_t *cover)
{
    if (!path || !cover) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(cover, 0, sizeof(*cover));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t id3[10];
    if (fread(id3, 1, sizeof(id3), fp) != sizeof(id3) || memcmp(id3, "ID3", 3) != 0) {
        fclose(fp);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t version = id3[3];
    uint32_t tag_size = read_syncsafe32(id3 + 6);
    uint32_t consumed = 0;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    while (consumed + 10 <= tag_size) {
        uint8_t frame[10];
        if (fread(frame, 1, sizeof(frame), fp) != sizeof(frame) || frame[0] == 0) {
            break;
        }
        consumed += sizeof(frame);

        uint32_t frame_size = version == 4 ? read_syncsafe32(frame + 4) : read_be32(frame + 4);
        if (frame_size == 0 || consumed + frame_size > tag_size) {
            break;
        }
        if (memcmp(frame, "APIC", 4) != 0) {
            fseek(fp, frame_size, SEEK_CUR);
            consumed += frame_size;
            continue;
        }

        uint8_t *payload = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!payload) {
            payload = heap_caps_malloc(frame_size, MALLOC_CAP_8BIT);
        }
        if (!payload) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        if (fread(payload, 1, frame_size, fp) != frame_size) {
            heap_caps_free(payload);
            result = ESP_FAIL;
            break;
        }

        size_t pos = 1;
        while (pos < frame_size && payload[pos] != 0) pos++;
        pos++;
        if (pos >= frame_size) {
            heap_caps_free(payload);
            break;
        }
        pos++; /* picture type */
        uint8_t encoding = payload[0];
        if (encoding == 1 || encoding == 2) {
            while (pos + 1 < frame_size && (payload[pos] != 0 || payload[pos + 1] != 0)) pos += 2;
            pos += 2;
        } else {
            while (pos < frame_size && payload[pos] != 0) pos++;
            pos++;
        }

        if (pos >= frame_size || frame_size - pos < 4) {
            heap_caps_free(payload);
            break;
        }
        const uint8_t *image = payload + pos;
        size_t image_size = frame_size - pos;
        static const uint8_t png_signature[8] = {
            0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
        };
        if (image_size >= sizeof(png_signature) &&
            memcmp(image, png_signature, sizeof(png_signature)) == 0) {
            result = decode_png_rgb565_alpha(image, image_size, cover);
        } else {
            result = decode_jpeg_rgb565(image, image_size, cover);
        }
        heap_caps_free(payload);
        break;
    }

    fclose(fp);
    return result;
}
