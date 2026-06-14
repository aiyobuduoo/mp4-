#include "mp3_duration.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define MP3_ID3_HEADER_SIZE 10U
#define MP3_FRAME_HEADER_SIZE 4U
#define MP3_SCAN_BUFFER_SIZE 4096U
#define MP3_SCAN_BUFFER_TOTAL_SIZE (MP3_SCAN_BUFFER_SIZE + MP3_FRAME_HEADER_SIZE)

static uint8_t *s_scan_buffer;

static esp_err_t ensure_scan_buffer(void)
{
    if (s_scan_buffer == NULL) {
        s_scan_buffer = heap_caps_malloc(MP3_SCAN_BUFFER_TOTAL_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_scan_buffer != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

typedef struct {
    uint32_t bitrate_bps;
    uint32_t sample_rate_hz;
    uint16_t samples_per_frame;
    uint8_t version;
    bool mono;
} mp3_frame_info_t;

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           data[3];
}

static uint32_t read_syncsafe32(const uint8_t *data)
{
    return ((uint32_t)(data[0] & 0x7F) << 21) |
           ((uint32_t)(data[1] & 0x7F) << 14) |
           ((uint32_t)(data[2] & 0x7F) << 7) |
           (data[3] & 0x7F);
}

static bool parse_frame_header(const uint8_t *header, mp3_frame_info_t *frame)
{
    static const uint16_t mpeg1_layer3_bitrates_kbps[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static const uint16_t mpeg2_layer3_bitrates_kbps[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };
    static const uint32_t sample_rates_hz[3] = {44100, 48000, 32000};

    uint32_t value = read_be32(header);
    uint8_t version_bits = (value >> 19) & 0x03;
    uint8_t layer_bits = (value >> 17) & 0x03;
    uint8_t bitrate_index = (value >> 12) & 0x0F;
    uint8_t sample_rate_index = (value >> 10) & 0x03;

    if ((value & 0xFFE00000U) != 0xFFE00000U ||
        version_bits == 1 || layer_bits != 1 ||
        bitrate_index == 0 || bitrate_index == 15 ||
        sample_rate_index == 3) {
        return false;
    }

    uint32_t sample_rate = sample_rates_hz[sample_rate_index];
    if (version_bits == 2) {
        sample_rate /= 2;
    } else if (version_bits == 0) {
        sample_rate /= 4;
    }

    frame->version = version_bits;
    frame->sample_rate_hz = sample_rate;
    frame->samples_per_frame = version_bits == 3 ? 1152 : 576;
    frame->mono = ((value >> 6) & 0x03) == 3;
    frame->bitrate_bps =
        (version_bits == 3 ? mpeg1_layer3_bitrates_kbps[bitrate_index]
                           : mpeg2_layer3_bitrates_kbps[bitrate_index]) * 1000U;
    return true;
}

static esp_err_t find_first_frame(FILE *file, uint64_t start_offset,
                                  uint64_t *frame_offset, mp3_frame_info_t *frame)
{
    esp_err_t err = ensure_scan_buffer();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *buffer = s_scan_buffer;
    uint64_t offset = start_offset;
    size_t carry = 0;

    if (fseek(file, (long)start_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    while (true) {
        size_t read = fread(buffer + carry, 1, MP3_SCAN_BUFFER_SIZE, file);
        size_t total = carry + read;

        for (size_t i = 0; i + MP3_FRAME_HEADER_SIZE <= total; i++) {
            if (parse_frame_header(buffer + i, frame)) {
                *frame_offset = offset - carry + i;
                return ESP_OK;
            }
        }

        if (read == 0) {
            return ESP_ERR_NOT_FOUND;
        }

        carry = total < 3 ? total : 3;
        memmove(buffer, buffer + total - carry, carry);
        offset += read;
    }
}

static bool read_xing_duration(FILE *file, uint64_t frame_offset,
                               const mp3_frame_info_t *frame, mp3_duration_info_t *info)
{
    uint32_t side_info_size;
    if (frame->version == 3) {
        side_info_size = frame->mono ? 17U : 32U;
    } else {
        side_info_size = frame->mono ? 9U : 17U;
    }

    uint64_t xing_offset = frame_offset + MP3_FRAME_HEADER_SIZE + side_info_size;
    uint8_t header[12];
    if (fseek(file, (long)xing_offset, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return false;
    }

    if (memcmp(header, "Xing", 4) != 0 && memcmp(header, "Info", 4) != 0) {
        return false;
    }

    uint32_t flags = read_be32(header + 4);
    if ((flags & 0x01U) == 0) {
        return false;
    }

    uint32_t frame_count = read_be32(header + 8);
    if (frame_count == 0) {
        return false;
    }

    info->audio_frame_count = frame_count;
    info->duration_ms = (uint32_t)(((uint64_t)frame_count * frame->samples_per_frame * 1000U) /
                                   frame->sample_rate_hz);
    info->source = MP3_DURATION_SOURCE_XING;
    return true;
}

static bool read_vbri_duration(FILE *file, uint64_t frame_offset,
                               const mp3_frame_info_t *frame, mp3_duration_info_t *info)
{
    uint8_t header[18];
    uint64_t vbri_offset = frame_offset + MP3_FRAME_HEADER_SIZE + 32U;
    if (fseek(file, (long)vbri_offset, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "VBRI", 4) != 0) {
        return false;
    }

    uint32_t frame_count = read_be32(header + 14);
    if (frame_count == 0) {
        return false;
    }

    info->audio_frame_count = frame_count;
    info->duration_ms = (uint32_t)(((uint64_t)frame_count * frame->samples_per_frame * 1000U) /
                                   frame->sample_rate_hz);
    info->source = MP3_DURATION_SOURCE_VBRI;
    return true;
}

esp_err_t mp3_duration_read(const char *path, mp3_duration_info_t *info)
{
    if (path == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t id3_header[MP3_ID3_HEADER_SIZE];
    uint64_t audio_start = 0;
    if (fread(id3_header, 1, sizeof(id3_header), file) == sizeof(id3_header) &&
        memcmp(id3_header, "ID3", 3) == 0) {
        audio_start = MP3_ID3_HEADER_SIZE + read_syncsafe32(id3_header + 6);
        if ((id3_header[5] & 0x10U) != 0) {
            audio_start += MP3_ID3_HEADER_SIZE;
        }
    }

    uint64_t frame_offset;
    mp3_frame_info_t frame;
    esp_err_t err = find_first_frame(file, audio_start, &frame_offset, &frame);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    info->sample_rate_hz = frame.sample_rate_hz;
    info->bitrate_bps = frame.bitrate_bps;

    if (read_xing_duration(file, frame_offset, &frame, info) ||
        read_vbri_duration(file, frame_offset, &frame, info)) {
        fclose(file);
        return ESP_OK;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    long file_size = ftell(file);
    fclose(file);
    if (file_size <= 0 || (uint64_t)file_size <= frame_offset || frame.bitrate_bps == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t audio_bytes = (uint64_t)file_size - frame_offset;
    info->duration_ms = (uint32_t)((audio_bytes * 8U * 1000U) / frame.bitrate_bps);
    info->source = MP3_DURATION_SOURCE_CBR;
    return ESP_OK;
}

const char *mp3_duration_source_name(mp3_duration_source_t source)
{
    switch (source) {
    case MP3_DURATION_SOURCE_XING:
        return "Xing/Info";
    case MP3_DURATION_SOURCE_VBRI:
        return "VBRI";
    case MP3_DURATION_SOURCE_CBR:
        return "CBR estimate";
    default:
        return "none";
    }
}
