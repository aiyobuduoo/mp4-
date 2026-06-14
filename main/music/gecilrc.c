#include "gecilrc.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sd.h"

#define LRC_DIR SDCARD_MOUNT_POINT "/geci"
#define LRC_MAX_FILE_SIZE (64 * 1024)
#define LRC_MAX_LINES 512
#define LRC_PATH_SIZE 512
#define LRC_NAME_SIZE 512

typedef struct {
    uint32_t time_ms;
    const char *text;
} lrc_line_t;

static const char *TAG = "gecilrc";
static char *s_file_buffer;
static char *s_path_buffer;
static char *s_best_path;
static char *s_name_buffer;
static char *s_title_key;
static char *s_artist_key;
static lrc_line_t *s_lines;
static size_t s_line_count;
static bool s_loaded;

static bool allocate_buffers(void)
{
    if (s_file_buffer) return true;
    s_file_buffer = heap_caps_malloc(LRC_MAX_FILE_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_path_buffer = heap_caps_malloc(LRC_PATH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_best_path = heap_caps_malloc(LRC_PATH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_name_buffer = heap_caps_malloc(LRC_NAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_title_key = heap_caps_malloc(LRC_NAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_artist_key = heap_caps_malloc(LRC_NAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lines = heap_caps_calloc(LRC_MAX_LINES, sizeof(*s_lines), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_file_buffer && s_path_buffer && s_best_path && s_name_buffer &&
           s_title_key && s_artist_key && s_lines;
}

static bool has_lrc_extension(const char *name)
{
    size_t length = strlen(name);
    if (length >= 4 && strcasecmp(name + length - 4, ".lrc") == 0) return true;
    return length >= 8 && strcasecmp(name + length - 8, ".lrc.txt") == 0;
}

static void normalize_key(const char *source, char *out, size_t out_size)
{
    size_t write = 0;
    for (const unsigned char *p = (const unsigned char *)source; *p && write + 1 < out_size; p++) {
        if (*p < 0x80) {
            if (isspace(*p) || *p == '-' || *p == '_' || *p == '(' || *p == ')' ||
                *p == '[' || *p == ']' || *p == '.') {
                continue;
            }
            out[write++] = (char)tolower(*p);
        } else {
            out[write++] = (char)*p;
        }
    }
    out[write] = '\0';
}

static int match_score(const char *filename)
{
    snprintf(s_name_buffer, LRC_NAME_SIZE, "%s", filename);
    size_t length = strlen(s_name_buffer);
    if (length >= 8 && strcasecmp(s_name_buffer + length - 8, ".lrc.txt") == 0) {
        s_name_buffer[length - 8] = '\0';
    } else if (length >= 4 && strcasecmp(s_name_buffer + length - 4, ".lrc") == 0) {
        s_name_buffer[length - 4] = '\0';
    }

    char *lyrics_suffix = strstr(s_name_buffer, "-歌词");
    if (lyrics_suffix) *lyrics_suffix = '\0';
    normalize_key(s_name_buffer, s_name_buffer, LRC_NAME_SIZE);

    bool title_match = s_title_key[0] && strstr(s_name_buffer, s_title_key);
    bool artist_match = s_artist_key[0] && strstr(s_name_buffer, s_artist_key);
    if (title_match && artist_match) return 100;
    if (title_match) return 60;
    if (s_name_buffer[0] && strcmp(s_name_buffer, s_title_key) == 0) return 80;
    return 0;
}

static bool parse_timestamp(const char *text, uint32_t *time_ms, const char **end)
{
    if (text[0] != '[' || !isdigit((unsigned char)text[1]) || !isdigit((unsigned char)text[2]) ||
        text[3] != ':' || !isdigit((unsigned char)text[4]) || !isdigit((unsigned char)text[5])) {
        return false;
    }
    uint32_t minutes = (uint32_t)(text[1] - '0') * 10 + (uint32_t)(text[2] - '0');
    uint32_t seconds = (uint32_t)(text[4] - '0') * 10 + (uint32_t)(text[5] - '0');
    uint32_t fraction_ms = 0;
    const char *p = text + 6;
    if (*p == '.') {
        p++;
        if (isdigit((unsigned char)*p)) fraction_ms = (uint32_t)(*p++ - '0') * 100;
        if (isdigit((unsigned char)*p)) fraction_ms += (uint32_t)(*p++ - '0') * 10;
        if (isdigit((unsigned char)*p)) fraction_ms += (uint32_t)(*p++ - '0');
    }
    if (*p != ']') return false;
    *time_ms = (minutes * 60 + seconds) * 1000 + fraction_ms;
    *end = p + 1;
    return true;
}

static int compare_lines(const void *a, const void *b)
{
    const lrc_line_t *line_a = a;
    const lrc_line_t *line_b = b;
    return line_a->time_ms < line_b->time_ms ? -1 : line_a->time_ms > line_b->time_ms;
}

static void parse_lrc_buffer(void)
{
    s_line_count = 0;
    char *line = s_file_buffer;
    while (*line && s_line_count < LRC_MAX_LINES) {
        char *next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }
        size_t length = strlen(line);
        if (length && line[length - 1] == '\r') line[length - 1] = '\0';

        const char *cursor = line;
        uint32_t times[8];
        size_t time_count = 0;
        while (time_count < 8) {
            uint32_t time_ms;
            const char *end;
            if (!parse_timestamp(cursor, &time_ms, &end)) break;
            times[time_count++] = time_ms;
            cursor = end;
        }
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (time_count && *cursor) {
            for (size_t i = 0; i < time_count && s_line_count < LRC_MAX_LINES; i++) {
                s_lines[s_line_count++] = (lrc_line_t){.time_ms = times[i], .text = cursor};
            }
        }
        if (!next) break;
        line = next;
    }
    qsort(s_lines, s_line_count, sizeof(s_lines[0]), compare_lines);
}

esp_err_t gecilrc_init(void)
{
    return allocate_buffers() ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gecilrc_load_for_track(const char *title, const char *artist)
{
    if (!title || !allocate_buffers()) return ESP_ERR_INVALID_ARG;
    s_loaded = false;
    s_line_count = 0;
    s_best_path[0] = '\0';
    normalize_key(title, s_title_key, LRC_NAME_SIZE);
    normalize_key(artist ? artist : "", s_artist_key, LRC_NAME_SIZE);

    DIR *dir = opendir(LRC_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open " LRC_DIR);
        return ESP_ERR_NOT_FOUND;
    }
    int best_score = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!has_lrc_extension(entry->d_name)) continue;
        int score = match_score(entry->d_name);
        if (score > best_score) {
            best_score = score;
            snprintf(s_best_path, LRC_PATH_SIZE, "%s/%s", LRC_DIR, entry->d_name);
            if (score == 100) break;
        }
    }
    closedir(dir);
    if (!s_best_path[0]) {
        ESP_LOGI(TAG, "No LRC match: %s / %s", title, artist ? artist : "");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(s_best_path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    size_t bytes = fread(s_file_buffer, 1, LRC_MAX_FILE_SIZE, file);
    fclose(file);
    s_file_buffer[bytes] = '\0';
    parse_lrc_buffer();
    s_loaded = s_line_count > 0;
    ESP_LOGI(TAG, "LRC matched: %s, lines=%u", s_best_path, (unsigned)s_line_count);
    return s_loaded ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

const char *gecilrc_get_line(uint32_t position_ms)
{
    if (!s_loaded || !s_line_count) return NULL;
    size_t left = 0;
    size_t right = s_line_count;
    while (left < right) {
        size_t middle = left + (right - left) / 2;
        if (s_lines[middle].time_ms <= position_ms) left = middle + 1;
        else right = middle;
    }
    return left ? s_lines[left - 1].text : NULL;
}

const char *gecilrc_get_matched_path(void)
{
    return s_loaded ? s_best_path : NULL;
}

size_t gecilrc_get_line_count(void)
{
    return s_line_count;
}

bool gecilrc_is_loaded(void)
{
    return s_loaded;
}

