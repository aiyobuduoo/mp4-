#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "es8311_code.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gecilrc.h"
#include "MP3.h"
#include "mp3_cover.h"
#include "mp3_duration.h"
#include "progress_seek.h"
#include "sd.h"
#include "ui/ui.h"

#define MUSIC_DIR SDCARD_MOUNT_POINT "/music"
#define MAX_TRACKS 128
#define TRACK_STRING_POOL_SIZE (64 * 1024)
#define ID3_FRAME_BUFFER_SIZE 4096
#define TEXT_BUFFER_SIZE 512
#define RESOURCE_TASK_STACK_SIZE (8 * 1024)
#define DEFAULT_VOLUME 5
#define VOLUME_PANEL_X (-369)
#define VOLUME_PANEL_HIDDEN_X (-395)
#define PLAYLIST_SHEET_Y 42
#define PLAYLIST_SHEET_H 438
#define PLAYLIST_ROW_H 68

typedef struct {
    char *path;
    char *title;
    char *artist;
} music_track_t;

typedef struct {
    size_t track_index;
    uint32_t generation;
    bool autoplay;
} resource_job_t;

typedef struct {
    size_t track_index;
    uint32_t generation;
    bool autoplay;
    uint32_t duration_ms;
    bool lyrics_loaded;
    mp3_cover_t cover;
} resource_result_t;

static const char *TAG = "ui_events";
static music_track_t *s_tracks;
static char *s_string_pool;
static size_t s_string_pool_used;
static uint8_t *s_id3_frame_buffer;
static char *s_title_buffer;
static char *s_artist_buffer;
static char *s_fallback_title_buffer;
static char *s_fallback_artist_buffer;
static char *s_path_buffer;
static size_t s_track_count;
static size_t s_track_index;
static bool s_playing;
static bool s_decoder_seen_playing;
static bool s_random_mode;
static uint32_t s_track_duration_ms;
static uint32_t s_resume_position_ms;
static const char *s_current_lyric;
static lv_timer_t *s_progress_timer;
static QueueHandle_t s_resource_job_queue;
static QueueHandle_t s_resource_result_queue;
static uint32_t s_resource_generation;
static bool s_resources_loading;
static bool s_resources_ready;
static bool s_initialized;
static int32_t s_cover_angle;

static mp3_cover_t s_cover;
static lv_img_dsc_t s_cover_dsc;

static lv_obj_t *s_playlist_backdrop;
static lv_obj_t *s_playlist_sheet;
static lv_obj_t *s_playlist_list;
static lv_obj_t *s_playlist_count_label;
static lv_obj_t *s_playlist_rows[MAX_TRACKS];
static lv_obj_t *s_playlist_numbers[MAX_TRACKS];
static lv_obj_t *s_playlist_titles[MAX_TRACKS];
static lv_obj_t *s_playlist_artists[MAX_TRACKS];
static lv_obj_t *s_playlist_indicators[MAX_TRACKS];
static bool s_playlist_open;

static void request_track_resources(bool autoplay);
static void refresh_playlist(void);

static void update_timeui_music_panel(void)
{
    if (!ui_musicpa || !ui_elseev || !ui_gemingxinxi || !ui_Label14) return;

    if (!s_playing || !s_track_count) {
        lv_obj_add_flag(ui_musicpa, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_elseev, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const music_track_t *track = &s_tracks[s_track_index];
    lv_label_set_text_fmt(ui_gemingxinxi, "%s - %s", track->title, track->artist);
    lv_label_set_text(ui_Label14, s_current_lyric ? s_current_lyric :
                     (s_resources_ready ? "No lyrics" : "Loading..."));
    lv_obj_add_flag(ui_elseev, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_musicpa, LV_OBJ_FLAG_HIDDEN);
}

static void cover_rotation_set_angle(void *object, int32_t angle)
{
    s_cover_angle = angle % 3600;
    lv_img_set_angle((lv_obj_t *)object, s_cover_angle);
}

static void update_cover_rotation(void)
{
    if (!ui_fengmian) return;

    if (!s_playing) {
        lv_anim_del(ui_fengmian, cover_rotation_set_angle);
        return;
    }
    if (lv_anim_get(ui_fengmian, cover_rotation_set_angle)) {
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ui_fengmian);
    lv_anim_set_exec_cb(&animation, cover_rotation_set_angle);
    lv_anim_set_values(&animation, s_cover_angle, s_cover_angle + 3600);
    lv_anim_set_time(&animation, 5000);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);
}

static void update_play_button_image(void)
{
    lv_obj_set_style_bg_img_src(ui_Button1,
                                s_playing ? &ui_img_bf1_png : &ui_img_1256964955,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    update_cover_rotation();
    update_timeui_music_panel();
}

static void update_play_mode_image(void)
{
    lv_obj_set_style_bg_img_src(ui_bofangsuij,
                                s_random_mode ? &ui_img_suiji_png : &ui_img_shunxu_png,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

static size_t next_track_index(void)
{
    if (!s_track_count) return 0;
    if (!s_random_mode || s_track_count == 1) {
        return (s_track_index + 1) % s_track_count;
    }
    size_t next;
    do {
        next = esp_random() % s_track_count;
    } while (next == s_track_index);
    return next;
}

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

static bool allocate_music_library(void)
{
    s_tracks = heap_caps_calloc(MAX_TRACKS, sizeof(*s_tracks), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_string_pool = heap_caps_malloc(TRACK_STRING_POOL_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_id3_frame_buffer = heap_caps_malloc(ID3_FRAME_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_title_buffer = heap_caps_malloc(TEXT_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_artist_buffer = heap_caps_malloc(TEXT_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fallback_title_buffer = heap_caps_malloc(TEXT_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fallback_artist_buffer = heap_caps_malloc(TEXT_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_path_buffer = heap_caps_malloc(TEXT_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_tracks && s_string_pool && s_id3_frame_buffer && s_title_buffer && s_artist_buffer &&
           s_fallback_title_buffer && s_fallback_artist_buffer && s_path_buffer;
}

static char *pool_strdup(const char *text)
{
    size_t length = strlen(text) + 1;
    if (!s_string_pool || s_string_pool_used + length > TRACK_STRING_POOL_SIZE) {
        return NULL;
    }
    char *copy = s_string_pool + s_string_pool_used;
    memcpy(copy, text, length);
    s_string_pool_used += length;
    return copy;
}

static bool has_mp3_extension(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && name[len - 4] == '.' &&
           tolower((unsigned char)name[len - 3]) == 'm' &&
           tolower((unsigned char)name[len - 2]) == 'p' &&
           tolower((unsigned char)name[len - 1]) == '3';
}

static void trim_text(char *text)
{
    char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != text) memmove(text, start, strlen(start) + 1);

    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1])) text[--len] = '\0';
}

static size_t append_utf8(char *out, size_t out_size, size_t pos, uint32_t cp)
{
    if (cp < 0x80 && pos + 1 < out_size) {
        out[pos++] = (char)cp;
    } else if (cp < 0x800 && pos + 2 < out_size) {
        out[pos++] = (char)(0xc0 | (cp >> 6));
        out[pos++] = (char)(0x80 | (cp & 0x3f));
    } else if (pos + 3 < out_size) {
        out[pos++] = (char)(0xe0 | (cp >> 12));
        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[pos++] = (char)(0x80 | (cp & 0x3f));
    }
    return pos;
}

static void decode_id3_text(const uint8_t *data, size_t size, char *out, size_t out_size)
{
    size_t write_pos = 0;
    if (!size || out_size < 2) {
        if (out_size) out[0] = '\0';
        return;
    }

    uint8_t encoding = data[0];
    if (encoding == 0 || encoding == 3) {
        size_t copy = size - 1;
        if (copy >= out_size) copy = out_size - 1;
        memcpy(out, data + 1, copy);
        out[copy] = '\0';
        trim_text(out);
        return;
    }

    bool big_endian = encoding == 2;
    size_t pos = 1;
    if (encoding == 1 && size >= 3) {
        if (data[1] == 0xfe && data[2] == 0xff) big_endian = true;
        if ((data[1] == 0xff && data[2] == 0xfe) || (data[1] == 0xfe && data[2] == 0xff)) pos = 3;
    }
    while (pos + 1 < size) {
        uint16_t cp = big_endian ? ((uint16_t)data[pos] << 8) | data[pos + 1]
                                 : ((uint16_t)data[pos + 1] << 8) | data[pos];
        pos += 2;
        if (!cp) break;
        write_pos = append_utf8(out, out_size, write_pos, cp);
    }
    out[write_pos] = '\0';
    trim_text(out);
}

static void metadata_from_filename(const char *filename, char *title, char *artist)
{
    snprintf(title, TEXT_BUFFER_SIZE, "%s", filename);
    char *extension = strrchr(title, '.');
    if (extension) *extension = '\0';
    artist[0] = '\0';

    char *separator = strrchr(title, '-');
    if (separator) {
        *separator = '\0';
        snprintf(artist, TEXT_BUFFER_SIZE, "%s", separator + 1);
        trim_text(title);
        trim_text(artist);
    }
    if (!artist[0]) snprintf(artist, TEXT_BUFFER_SIZE, "Unknown Artist");
}

static void read_id3_metadata(const char *path, char *title, char *artist)
{
    FILE *file = fopen(path, "rb");
    if (!file) return;

    uint8_t header[10];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
        fclose(file);
        return;
    }

    uint8_t version = header[3];
    uint32_t tag_size = read_syncsafe32(header + 6);
    uint32_t consumed = 0;
    while (consumed + 10 <= tag_size && (!title[0] || !artist[0])) {
        uint8_t frame_header[10];
        if (fread(frame_header, 1, sizeof(frame_header), file) != sizeof(frame_header) || frame_header[0] == 0) break;
        consumed += sizeof(frame_header);

        uint32_t frame_size = version == 4 ? read_syncsafe32(frame_header + 4) : read_be32(frame_header + 4);
        if (!frame_size || consumed + frame_size > tag_size) break;

        bool wanted = memcmp(frame_header, "TIT2", 4) == 0 || memcmp(frame_header, "TPE1", 4) == 0;
        if (!wanted || frame_size > ID3_FRAME_BUFFER_SIZE) {
            fseek(file, frame_size, SEEK_CUR);
            consumed += frame_size;
            continue;
        }
        if (fread(s_id3_frame_buffer, 1, frame_size, file) != frame_size) break;
        consumed += frame_size;
        if (memcmp(frame_header, "TIT2", 4) == 0) {
            decode_id3_text(s_id3_frame_buffer, frame_size, title, TEXT_BUFFER_SIZE);
        } else {
            decode_id3_text(s_id3_frame_buffer, frame_size, artist, TEXT_BUFFER_SIZE);
        }
    }
    fclose(file);
}

static int compare_tracks(const void *a, const void *b)
{
    const music_track_t *track_a = a;
    const music_track_t *track_b = b;
    return strcmp(track_a->title, track_b->title);
}

static void scan_tracks(void)
{
    if (!allocate_music_library()) {
        ESP_LOGE(TAG, "Cannot allocate music library in PSRAM");
        return;
    }

    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open " MUSIC_DIR);
        return;
    }

    struct dirent *entry;
    while (s_track_count < MAX_TRACKS && (entry = readdir(dir)) != NULL) {
        if (!has_mp3_extension(entry->d_name)) continue;

        snprintf(s_path_buffer, TEXT_BUFFER_SIZE, "%s/%s", MUSIC_DIR, entry->d_name);
        s_title_buffer[0] = '\0';
        s_artist_buffer[0] = '\0';
        read_id3_metadata(s_path_buffer, s_title_buffer, s_artist_buffer);
        if (!s_title_buffer[0] || !s_artist_buffer[0]) {
            metadata_from_filename(entry->d_name, s_fallback_title_buffer, s_fallback_artist_buffer);
            if (!s_title_buffer[0]) snprintf(s_title_buffer, TEXT_BUFFER_SIZE, "%s", s_fallback_title_buffer);
            if (!s_artist_buffer[0]) snprintf(s_artist_buffer, TEXT_BUFFER_SIZE, "%s", s_fallback_artist_buffer);
        }

        music_track_t *track = &s_tracks[s_track_count];
        track->path = pool_strdup(s_path_buffer);
        track->title = pool_strdup(s_title_buffer);
        track->artist = pool_strdup(s_artist_buffer);
        if (!track->path || !track->title || !track->artist) {
            ESP_LOGW(TAG, "Music string pool full");
            break;
        }
        s_track_count++;
    }
    closedir(dir);
    qsort(s_tracks, s_track_count, sizeof(s_tracks[0]), compare_tracks);
    ESP_LOGI(TAG, "Found %u MP3 tracks, PSRAM strings=%u bytes",
             (unsigned)s_track_count, (unsigned)s_string_pool_used);
}

static void format_time(uint32_t duration_ms, char *out, size_t out_size)
{
    uint32_t seconds = duration_ms / 1000;
    snprintf(out, out_size, "%lu:%02lu",
             (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

static void apply_cover(mp3_cover_t *new_cover)
{
    if (s_cover.data) {
        lv_img_cache_invalidate_src(&s_cover_dsc);
        lv_img_set_src(ui_fengmian, NULL);
        mp3_cover_free(&s_cover);
        memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
    }
    if (!new_cover || !new_cover->data) {
        lv_img_set_src(ui_fengmian, &ui_img_1953031374);
        lv_obj_center(ui_fengmian);
        return;
    }
    s_cover = *new_cover;
    memset(new_cover, 0, sizeof(*new_cover));
    s_cover_dsc.header.cf = s_cover.has_alpha ? LV_IMG_CF_TRUE_COLOR_ALPHA : LV_IMG_CF_TRUE_COLOR;
    s_cover_dsc.header.w = s_cover.width;
    s_cover_dsc.header.h = s_cover.height;
    s_cover_dsc.data_size = s_cover.size;
    s_cover_dsc.data = s_cover.data;
    lv_img_set_src(ui_fengmian, &s_cover_dsc);
    lv_obj_set_size(ui_fengmian, 128, 128);
    lv_obj_center(ui_fengmian);
}

static void show_track_loading(void)
{
    if (!s_track_count) {
        lv_label_set_text(ui_geming, "No MP3 files");
        lv_label_set_text(ui_geshou, MUSIC_DIR);
        lv_label_set_text(ui_Label7, "0:00");
        return;
    }

    music_track_t *track = &s_tracks[s_track_index];
    s_track_duration_ms = 0;
    progress_seek_set_duration(0);
    s_current_lyric = NULL;
    lv_label_set_text(ui_geming, track->title);
    lv_label_set_text(ui_geshou, track->artist);
    lv_label_set_text(ui_jindu1, "0:00");
    lv_label_set_text(ui_Label7, "0:00");
    lv_label_set_text(ui_geci, "Loading...");
    lv_slider_set_value(ui_Slider1, 0, LV_ANIM_OFF);
}

static void resource_loader_task(void *arg)
{
    (void)arg;
    resource_job_t job;
    while (true) {
        if (xQueueReceive(s_resource_job_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        resource_result_t result = {
            .track_index = job.track_index,
            .generation = job.generation,
            .autoplay = job.autoplay,
        };
        if (job.track_index >= s_track_count) continue;

        mp3_stop_and_wait(2000);
        music_track_t *track = &s_tracks[job.track_index];
        mp3_duration_info_t duration;
        if (mp3_duration_read(track->path, &duration) == ESP_OK) {
            result.duration_ms = duration.duration_ms;
        }
        result.lyrics_loaded = gecilrc_load_for_track(track->title, track->artist) == ESP_OK;
        if (mp3_cover_read(track->path, &result.cover) != ESP_OK) {
            memset(&result.cover, 0, sizeof(result.cover));
        }

        resource_result_t old_result;
        if (xQueueReceive(s_resource_result_queue, &old_result, 0) == pdTRUE) {
            mp3_cover_free(&old_result.cover);
        }
        xQueueSend(s_resource_result_queue, &result, portMAX_DELAY);
    }
}

static void request_track_resources(bool autoplay)
{
    if (!s_track_count || !s_resource_job_queue) return;
    s_playing = false;
    s_resume_position_ms = 0;
    s_decoder_seen_playing = false;
    update_play_button_image();
    s_resources_loading = true;
    s_resources_ready = false;
    s_resource_generation++;
    show_track_loading();
    refresh_playlist();

    resource_job_t job = {
        .track_index = s_track_index,
        .generation = s_resource_generation,
        .autoplay = autoplay,
    };
    xQueueReset(s_resource_job_queue);
    xQueueSend(s_resource_job_queue, &job, 0);
}

static void apply_resource_result(resource_result_t *result)
{
    s_track_duration_ms = result->duration_ms;
    progress_seek_set_duration(s_track_duration_ms);
    char total[16];
    format_time(s_track_duration_ms, total, sizeof(total));
    lv_label_set_text(ui_Label7, total);
    if (result->lyrics_loaded) {
        const char *first_line = gecilrc_get_line(0);
        s_current_lyric = first_line;
        lv_label_set_text(ui_geci, first_line ? first_line : "...");
    } else {
        lv_label_set_text(ui_geci, "No lyrics");
    }
    apply_cover(&result->cover);
    s_resources_loading = false;
    s_resources_ready = true;
    if (result->autoplay) {
        mp3_play_file(s_tracks[s_track_index].path);
        s_playing = true;
        s_resume_position_ms = 0;
        s_decoder_seen_playing = false;
        update_play_button_image();
    }
}

static void progress_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_track_count) return;

    resource_result_t result;
    if (s_resource_result_queue && xQueueReceive(s_resource_result_queue, &result, 0) == pdTRUE) {
        if (result.generation == s_resource_generation && result.track_index == s_track_index) {
            apply_resource_result(&result);
        } else {
            mp3_cover_free(&result.cover);
        }
    }
    if (s_resources_loading) return;

    uint32_t position_ms = mp3_get_position_ms();
    if (s_track_duration_ms && position_ms > s_track_duration_ms) {
        position_ms = s_track_duration_ms;
    }

    progress_seek_update(position_ms);

    const char *lyric = gecilrc_get_line(position_ms);
    if (lyric && lyric != s_current_lyric) {
        s_current_lyric = lyric;
        lv_label_set_text(ui_geci, lyric);
        update_timeui_music_panel();
    }
    bool decoder_playing = mp3_is_playing();
    if (decoder_playing) {
        s_decoder_seen_playing = true;
    }
    if (s_playing && s_decoder_seen_playing && !decoder_playing && position_ms > 0) {
        s_playing = false;
        s_resume_position_ms = 0;
        s_decoder_seen_playing = false;
        update_play_button_image();
        s_track_index = next_track_index();
        request_track_resources(true);
    }
}

static void play_current(void)
{
    if (!s_track_count) return;
    if (!s_resources_ready) {
        request_track_resources(true);
        return;
    }

    if (s_resume_position_ms > 0 && s_track_duration_ms > 0) {
        size_t frame_offset;
        esp_err_t err = progress_seek_find_mp3_frame(s_tracks[s_track_index].path,
                                                      s_track_duration_ms,
                                                      s_resume_position_ms,
                                                      &frame_offset);
        if (err == ESP_OK) {
            mp3_play_file_from_offset(s_tracks[s_track_index].path, frame_offset,
                                      s_resume_position_ms);
        } else {
            ESP_LOGE(TAG, "Cannot resume at %lu ms: %s",
                     (unsigned long)s_resume_position_ms, esp_err_to_name(err));
            s_resume_position_ms = 0;
            mp3_play_file(s_tracks[s_track_index].path);
        }
    } else {
        mp3_play_file(s_tracks[s_track_index].path);
    }
    s_playing = true;
    s_resume_position_ms = 0;
    s_decoder_seen_playing = false;
    update_play_button_image();
}

static void refresh_playlist(void)
{
    if (!s_playlist_count_label) return;

    lv_label_set_text_fmt(s_playlist_count_label, "%u tracks", (unsigned)s_track_count);
    for (size_t index = 0; index < s_track_count; index++) {
        bool active = index == s_track_index;
        lv_obj_set_style_bg_color(s_playlist_rows[index],
                                  active ? lv_color_hex(0x203D39) : lv_color_hex(0x171B20),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(s_playlist_rows[index],
                                      active ? lv_color_hex(0x38B7A5) : lv_color_hex(0x252B31),
                                      LV_PART_MAIN);
        lv_obj_set_style_text_color(s_playlist_numbers[index],
                                    active ? lv_color_hex(0x38B7A5) : lv_color_hex(0x737D87),
                                    LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_playlist_indicators[index],
                                active ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    }
}

static void playlist_sheet_set_y(void *object, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)object, (lv_coord_t)y);
}

static void playlist_close_ready(lv_anim_t *animation)
{
    (void)animation;
    lv_obj_add_flag(s_playlist_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_playlist_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void close_playlist(void)
{
    if (!s_playlist_open || !s_playlist_sheet) return;
    s_playlist_open = false;
    lv_anim_del(s_playlist_sheet, playlist_sheet_set_y);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_playlist_sheet);
    lv_anim_set_exec_cb(&animation, playlist_sheet_set_y);
    lv_anim_set_values(&animation, lv_obj_get_y(s_playlist_sheet), 480);
    lv_anim_set_time(&animation, 220);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&animation, playlist_close_ready);
    lv_anim_start(&animation);
}

static void ui_event_playlist_close(lv_event_t *event)
{
    (void)event;
    close_playlist();
}

static void ui_event_playlist_gesture(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM) {
        lv_indev_wait_release(indev);
        close_playlist();
    }
}

static void ui_event_playlist_track(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_track_count) return;
    s_track_index = index;
    refresh_playlist();
    request_track_resources(true);
    close_playlist();
}

static void create_playlist_sheet(void)
{
    s_playlist_backdrop = lv_obj_create(ui_musicui);
    lv_obj_set_size(s_playlist_backdrop, 800, 480);
    lv_obj_set_pos(s_playlist_backdrop, 0, 0);
    lv_obj_clear_flag(s_playlist_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_playlist_backdrop, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_playlist_backdrop, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_playlist_backdrop, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_playlist_backdrop, LV_OPA_60, LV_PART_MAIN);
    lv_obj_add_event_cb(s_playlist_backdrop, ui_event_playlist_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_playlist_backdrop, LV_OBJ_FLAG_HIDDEN);

    s_playlist_sheet = lv_obj_create(ui_musicui);
    lv_obj_set_size(s_playlist_sheet, 800, PLAYLIST_SHEET_H);
    lv_obj_set_pos(s_playlist_sheet, 0, 480);
    lv_obj_clear_flag(s_playlist_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_playlist_sheet, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_playlist_sheet, lv_color_hex(0x101419), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_playlist_sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_playlist_sheet, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_playlist_sheet, lv_color_hex(0x2B343D), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_playlist_sheet, 28, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_playlist_sheet, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_playlist_sheet, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(s_playlist_sheet, ui_event_playlist_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(s_playlist_sheet, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *handle = lv_obj_create(s_playlist_sheet);
    lv_obj_set_size(handle, 72, 5);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -8);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(handle, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0x52606B), LV_PART_MAIN);

    lv_obj_t *heading = lv_label_create(s_playlist_sheet);
    lv_label_set_text(heading, "Music Library");
    lv_obj_set_style_text_color(heading, lv_color_hex(0xF5F7F8), LV_PART_MAIN);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 4, 10);

    s_playlist_count_label = lv_label_create(s_playlist_sheet);
    lv_obj_set_style_text_color(s_playlist_count_label, lv_color_hex(0x7D8993), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_playlist_count_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_playlist_count_label, LV_ALIGN_TOP_LEFT, 6, 40);

    lv_obj_t *close_button = lv_btn_create(s_playlist_sheet);
    lv_obj_set_size(close_button, 42, 42);
    lv_obj_align(close_button, LV_ALIGN_TOP_RIGHT, -2, 5);
    lv_obj_set_style_radius(close_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(close_button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x252C33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x36414A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(close_button, ui_event_playlist_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xDCE2E6), LV_PART_MAIN);
    lv_obj_center(close_label);

    s_playlist_list = lv_obj_create(s_playlist_sheet);
    lv_obj_set_size(s_playlist_list, 760, 350);
    lv_obj_align(s_playlist_list, LV_ALIGN_BOTTOM_MID, 0, 4);
    lv_obj_set_scroll_dir(s_playlist_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_playlist_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(s_playlist_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_playlist_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_playlist_list, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_playlist_list, 18, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_playlist_list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_playlist_list, lv_color_hex(0x0C1014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_playlist_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_playlist_list, lv_color_hex(0x38B7A5), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_playlist_list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_playlist_list, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(s_playlist_list, ui_event_playlist_gesture, LV_EVENT_GESTURE, NULL);

    for (size_t index = 0; index < s_track_count; index++) {
        lv_obj_t *button = lv_btn_create(s_playlist_list);
        s_playlist_rows[index] = button;
        lv_obj_set_size(button, lv_pct(100), PLAYLIST_ROW_H);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(button, 14, LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x171B20), LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x294944), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(button, lv_color_hex(0x252B31), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(button, ui_event_playlist_track, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)index);

        s_playlist_numbers[index] = lv_label_create(button);
        lv_label_set_text_fmt(s_playlist_numbers[index], "%02u", (unsigned)(index + 1));
        lv_obj_set_style_text_font(s_playlist_numbers[index], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_playlist_numbers[index], LV_ALIGN_LEFT_MID, 0, 0);

        s_playlist_titles[index] = lv_label_create(button);
        lv_obj_set_width(s_playlist_titles[index], 590);
        lv_label_set_long_mode(s_playlist_titles[index], LV_LABEL_LONG_DOT);
        lv_label_set_text(s_playlist_titles[index], s_tracks[index].title);
        lv_obj_set_style_text_color(s_playlist_titles[index], lv_color_hex(0xF4F6F7), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_playlist_titles[index], &ui_font_Font3, LV_PART_MAIN);
        lv_obj_align(s_playlist_titles[index], LV_ALIGN_TOP_LEFT, 48, -5);

        s_playlist_artists[index] = lv_label_create(button);
        lv_obj_set_width(s_playlist_artists[index], 590);
        lv_label_set_long_mode(s_playlist_artists[index], LV_LABEL_LONG_DOT);
        lv_label_set_text(s_playlist_artists[index], s_tracks[index].artist);
        lv_obj_set_style_text_color(s_playlist_artists[index], lv_color_hex(0x82909A), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_playlist_artists[index], &ui_font_Font3, LV_PART_MAIN);
        lv_obj_align(s_playlist_artists[index], LV_ALIGN_BOTTOM_LEFT, 48, 5);

        s_playlist_indicators[index] = lv_obj_create(button);
        lv_obj_set_size(s_playlist_indicators[index], 5, 32);
        lv_obj_align(s_playlist_indicators[index], LV_ALIGN_RIGHT_MID, 1, 0);
        lv_obj_clear_flag(s_playlist_indicators[index], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_playlist_indicators[index], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_playlist_indicators[index], 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_playlist_indicators[index], lv_color_hex(0x38B7A5), LV_PART_MAIN);
    }
}

static void ui_event_open_playlist(lv_event_t *event)
{
    (void)event;
    if (!s_playlist_sheet || s_playlist_open) return;
    refresh_playlist();
    lv_anim_del(s_playlist_sheet, playlist_sheet_set_y);
    lv_obj_move_foreground(s_playlist_backdrop);
    lv_obj_move_foreground(s_playlist_sheet);
    lv_obj_clear_flag(s_playlist_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_playlist_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_playlist_sheet, 480);
    s_playlist_open = true;

    if (s_track_count && s_playlist_rows[s_track_index]) {
        lv_obj_update_layout(s_playlist_list);
        lv_obj_scroll_to_view(s_playlist_rows[s_track_index], LV_ANIM_OFF);
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_playlist_sheet);
    lv_anim_set_exec_cb(&animation, playlist_sheet_set_y);
    lv_anim_set_values(&animation, 480, PLAYLIST_SHEET_Y);
    lv_anim_set_time(&animation, 300);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static void ui_event_play_toggle(lv_event_t *event)
{
    (void)event;
    if (!s_track_count) return;
    if (s_playing) {
        s_resume_position_ms = mp3_get_position_ms();
        mp3_stop_and_wait(2000);
        s_playing = false;
        s_decoder_seen_playing = false;
        update_play_button_image();
    } else {
        play_current();
    }
}

static void ui_event_toggle_play_mode(lv_event_t *event)
{
    (void)event;
    s_random_mode = !s_random_mode;
    update_play_mode_image();
}

static void ui_event_volume_changed(lv_event_t *event)
{
    (void)event;
    uint8_t volume = (uint8_t)lv_slider_get_value(ui_Slider2);
    esp_err_t err = es8311_codec_set_volume(volume);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot set volume to %u: %s", volume, esp_err_to_name(err));
    }
}

static void volume_panel_set_x(void *object, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)object, (lv_coord_t)x);
}

static void ui_event_volume_toggle(lv_event_t *event)
{
    (void)event;
    if (lv_obj_has_flag(ui_Panel2, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_x(ui_Panel2, VOLUME_PANEL_HIDDEN_X);
        lv_obj_set_style_opa(ui_Panel2, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(ui_Panel2, LV_OBJ_FLAG_HIDDEN);

        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, ui_Panel2);
        lv_anim_set_exec_cb(&animation, volume_panel_set_x);
        lv_anim_set_values(&animation, VOLUME_PANEL_HIDDEN_X, VOLUME_PANEL_X);
        lv_anim_set_time(&animation, 200);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
        lv_anim_start(&animation);
    } else {
        lv_obj_add_flag(ui_Panel2, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_event_next(lv_event_t *event)
{
    (void)event;
    if (!s_track_count) return;
    s_track_index = next_track_index();
    request_track_resources(true);
}

static void ui_event_previous(lv_event_t *event)
{
    (void)event;
    if (!s_track_count) return;
    s_track_index = s_track_index ? s_track_index - 1 : s_track_count - 1;
    request_track_resources(true);
}

static void ui_progress_seek_commit(uint32_t target_ms, void *user_data)
{
    (void)user_data;
    if (!s_track_count || !s_resources_ready || !s_track_duration_ms) return;
    if (target_ms >= s_track_duration_ms) {
        target_ms = s_track_duration_ms > 1000U ? s_track_duration_ms - 1000U : 0;
    }

    s_playing = false;
    s_resume_position_ms = 0;
    s_decoder_seen_playing = false;
    update_play_button_image();
    if (!mp3_stop_and_wait(2000)) {
        ESP_LOGE(TAG, "Cannot stop decoder before seek");
        return;
    }

    size_t frame_offset;
    esp_err_t err = progress_seek_find_mp3_frame(s_tracks[s_track_index].path,
                                                  s_track_duration_ms, target_ms,
                                                  &frame_offset);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot seek to %lu ms: %s",
                 (unsigned long)target_ms, esp_err_to_name(err));
        mp3_play_file(s_tracks[s_track_index].path);
        target_ms = 0;
    } else {
        mp3_play_file_from_offset(s_tracks[s_track_index].path, frame_offset, target_ms);
    }
    s_playing = true;
    s_resume_position_ms = 0;
    s_decoder_seen_playing = false;
    progress_seek_update(target_ms);
    update_play_button_image();
}

void ui_events_init(void)
{
    if (s_initialized) return;
    s_initialized = true;

    lv_obj_add_event_cb(ui_Button1, ui_event_play_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_xiayishou, ui_event_next, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_shangyishou, ui_event_previous, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_bofangsuij, ui_event_toggle_play_mode, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_bofangliebiao, ui_event_open_playlist, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(ui_gemingxinxi, 480);
    lv_label_set_long_mode(ui_gemingxinxi, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ui_Label14, 480);
    lv_label_set_long_mode(ui_Label14, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_remove_event_cb(ui_yinl, ui_event_yinl);
    lv_obj_add_event_cb(ui_yinl, ui_event_volume_toggle, LV_EVENT_CLICKED, NULL);
    lv_slider_set_range(ui_Slider2, 0, 100);
    lv_slider_set_value(ui_Slider2, DEFAULT_VOLUME, LV_ANIM_OFF);
    lv_obj_add_event_cb(ui_Slider2, ui_event_volume_changed, LV_EVENT_VALUE_CHANGED, NULL);
    progress_seek_init(ui_Slider1, ui_jindu1, ui_progress_seek_commit, NULL);
    update_play_button_image();
    update_play_mode_image();
    gecilrc_init();
    scan_tracks();
    s_resource_job_queue = xQueueCreate(1, sizeof(resource_job_t));
    s_resource_result_queue = xQueueCreate(1, sizeof(resource_result_t));
    if (!s_resource_job_queue || !s_resource_result_queue ||
        xTaskCreatePinnedToCoreWithCaps(resource_loader_task, "music_resource", RESOURCE_TASK_STACK_SIZE,
                                        NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start music resource loader");
    }
    create_playlist_sheet();
    refresh_playlist();
    s_progress_timer = lv_timer_create(progress_timer_cb, 200, NULL);
    request_track_resources(false);
}
