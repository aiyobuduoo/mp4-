#include "progress_seek.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

#define SEEK_SCAN_BUFFER_SIZE 8192U
#define SEEK_SCAN_LIMIT (256U * 1024U)

static lv_obj_t *s_slider;
static lv_obj_t *s_time_label;
static progress_seek_commit_cb_t s_commit_cb;
static void *s_commit_user_data;
static uint32_t s_duration_ms;
static bool s_dragging;
static uint8_t *s_scan_buffer;

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t read_syncsafe32(const uint8_t *data)
{
    return ((uint32_t)(data[0] & 0x7f) << 21) | ((uint32_t)(data[1] & 0x7f) << 14) |
           ((uint32_t)(data[2] & 0x7f) << 7) | (data[3] & 0x7f);
}

static bool mp3_frame_size(const uint8_t *header, uint32_t *frame_size)
{
    static const uint16_t mpeg1_bitrate_kbps[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static const uint16_t mpeg2_bitrate_kbps[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };
    static const uint32_t sample_rates[3] = {44100, 48000, 32000};

    uint32_t value = read_be32(header);
    uint8_t version = (value >> 19) & 0x03;
    uint8_t layer = (value >> 17) & 0x03;
    uint8_t bitrate_index = (value >> 12) & 0x0f;
    uint8_t sample_rate_index = (value >> 10) & 0x03;
    if ((value & 0xffe00000U) != 0xffe00000U || version == 1 || layer != 1 ||
        bitrate_index == 0 || bitrate_index == 15 || sample_rate_index == 3) {
        return false;
    }

    uint32_t sample_rate = sample_rates[sample_rate_index];
    if (version == 2) sample_rate /= 2;
    else if (version == 0) sample_rate /= 4;
    uint32_t bitrate = (version == 3 ? mpeg1_bitrate_kbps[bitrate_index]
                                     : mpeg2_bitrate_kbps[bitrate_index]) * 1000U;
    uint32_t padding = (value >> 9) & 1U;
    *frame_size = ((version == 3 ? 144U : 72U) * bitrate) / sample_rate + padding;
    return *frame_size >= 24;
}

static void format_time(uint32_t time_ms, char *out, size_t out_size)
{
    uint32_t seconds = time_ms / 1000U;
    lv_snprintf(out, out_size, "%lu:%02lu",
                (unsigned long)(seconds / 60U), (unsigned long)(seconds % 60U));
}

static uint32_t slider_target_ms(void)
{
    return s_duration_ms
               ? (uint32_t)(((uint64_t)lv_slider_get_value(s_slider) * s_duration_ms) / 1000U)
               : 0;
}

static void show_slider_time(void)
{
    char time_text[16];
    format_time(slider_target_ms(), time_text, sizeof(time_text));
    lv_label_set_text(s_time_label, time_text);
}

static void slider_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
        show_slider_time();
    } else if (code == LV_EVENT_VALUE_CHANGED && s_dragging) {
        show_slider_time();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_dragging) {
        uint32_t target_ms = slider_target_ms();
        s_dragging = false;
        if (s_commit_cb && s_duration_ms) {
            s_commit_cb(target_ms, s_commit_user_data);
        }
    }
}

void progress_seek_init(lv_obj_t *slider, lv_obj_t *time_label,
                        progress_seek_commit_cb_t commit_cb, void *user_data)
{
    s_slider = slider;
    s_time_label = time_label;
    s_commit_cb = commit_cb;
    s_commit_user_data = user_data;
    lv_slider_set_range(slider, 0, 1000);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(slider, 18);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_ALL, NULL);
}

void progress_seek_set_duration(uint32_t duration_ms)
{
    s_duration_ms = duration_ms;
}

void progress_seek_update(uint32_t position_ms)
{
    if (!s_slider || !s_time_label || s_dragging) return;
    if (s_duration_ms && position_ms > s_duration_ms) position_ms = s_duration_ms;
    int32_t value = s_duration_ms
                        ? (int32_t)(((uint64_t)position_ms * 1000U) / s_duration_ms)
                        : 0;
    lv_slider_set_value(s_slider, value, LV_ANIM_OFF);
    char time_text[16];
    format_time(position_ms, time_text, sizeof(time_text));
    lv_label_set_text(s_time_label, time_text);
}

bool progress_seek_is_dragging(void)
{
    return s_dragging;
}

esp_err_t progress_seek_find_mp3_frame(const char *path, uint32_t duration_ms,
                                       uint32_t target_ms, size_t *frame_offset)
{
    if (!path || !duration_ms || !frame_offset) return ESP_ERR_INVALID_ARG;
    if (!s_scan_buffer) {
        s_scan_buffer = heap_caps_malloc(SEEK_SCAN_BUFFER_SIZE + 4,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scan_buffer) return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long file_size_long = ftell(file);
    if (file_size_long <= 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t file_size = (size_t)file_size_long;
    size_t audio_start = 0;
    uint8_t id3_header[10];
    rewind(file);
    if (fread(id3_header, 1, sizeof(id3_header), file) == sizeof(id3_header) &&
        memcmp(id3_header, "ID3", 3) == 0) {
        audio_start = 10U + read_syncsafe32(id3_header + 6);
        if (id3_header[5] & 0x10U) audio_start += 10U;
    }

    size_t audio_size = file_size > audio_start ? file_size - audio_start : 0;
    size_t estimate = audio_start + (size_t)(((uint64_t)audio_size * target_ms) / duration_ms);
    if (estimate >= file_size) estimate = file_size - 1;
    if (fseek(file, (long)estimate, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    size_t scanned = 0;
    size_t carry = 0;
    while (scanned < SEEK_SCAN_LIMIT) {
        size_t read = fread(s_scan_buffer + carry, 1, SEEK_SCAN_BUFFER_SIZE, file);
        size_t total = carry + read;
        for (size_t i = 0; i + 4 <= total; i++) {
            uint32_t first_size;
            if (!mp3_frame_size(s_scan_buffer + i, &first_size)) continue;
            size_t absolute = estimate + scanned - carry + i;
            if (absolute + first_size + 4 > file_size) continue;

            uint8_t next_header[4];
            if (i + first_size + 4 <= total) {
                memcpy(next_header, s_scan_buffer + i + first_size, sizeof(next_header));
            } else {
                long restore = ftell(file);
                if (fseek(file, (long)(absolute + first_size), SEEK_SET) != 0 ||
                    fread(next_header, 1, sizeof(next_header), file) != sizeof(next_header)) {
                    continue;
                }
                fseek(file, restore, SEEK_SET);
            }
            uint32_t second_size;
            if (mp3_frame_size(next_header, &second_size)) {
                *frame_offset = absolute;
                fclose(file);
                return ESP_OK;
            }
        }
        if (!read) break;
        carry = total < 3 ? total : 3;
        memmove(s_scan_buffer, s_scan_buffer + total - carry, carry);
        scanned += read;
    }
    fclose(file);
    return ESP_ERR_NOT_FOUND;
}
