#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <dirent.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/jpeg_decode.h"
#include "cJSON.h"

#include "config.h"
#include "mjpeg_player.h"
#include "ui_demo.h"

#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))
#define MIN_VAL(a, b) ((a) < (b) ? (a) : (b))
#define PLAYER_MAX_VIDEOS 64
#define PLAYER_NAME_BUF   256
#define MJPEG_SD_PREFILL_BYTES (64 * 1024)
#define MJPEG_SD_READ_CHUNK    (32 * 1024)

static const char *TAG = "mjpeg_player";

typedef struct {
    uint8_t *cache_buf;
    size_t cache_size;
    size_t cache_capacity;
    bool cache_complete;
    bool cache_truncated;
    bool present_decode_directly;
    jpeg_decoder_handle_t decoder;
    uint8_t *decode_buf;
    size_t decode_buf_size;
    uint8_t *frame_buf;
    size_t frame_buf_size;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t aligned_width;
    uint32_t aligned_height;
    uint32_t display_width;
    uint32_t display_height;
} mjpeg_ctx_t;

typedef struct {
    int64_t decode_us;
    int64_t copy_us;
    int64_t present_us;
    uint32_t out_size;
    uint32_t width;
    uint32_t height;
} frame_perf_t;

typedef struct {
    SemaphoreHandle_t lock;
    char *base_url;
    char *stream_url;
    bool use_sd_source;
    char *sd_dir;
    char **videos;
    size_t video_count;
    int current_index;
    int pending_index;
    bool switch_requested;
    bool running;
    bool active;
} player_state_t;

static player_state_t s_player = {
    .current_index = -1,
    .pending_index = -1,
    .active = true,
};

static bool player_lock(TickType_t timeout)
{
    return s_player.lock && xSemaphoreTake(s_player.lock, timeout) == pdTRUE;
}

static void player_unlock(void)
{
    if (s_player.lock) {
        xSemaphoreGive(s_player.lock);
    }
}

static char *strdup_safe(const char *src)
{
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *dst = (char *)malloc(len + 1);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, len + 1);
    return dst;
}

static char *dup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        return NULL;
    }

    char *buf = (char *)malloc((size_t)need + 1);
    if (!buf) {
        return NULL;
    }

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    va_end(ap);
    return buf;
}

static char *url_encode_component(const char *src)
{
    if (!src) {
        return NULL;
    }

    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        bool safe = ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                     (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~');
        len += safe ? 1 : 3;
    }

    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }

    char *w = out;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        bool safe = ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                     (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~');
        if (safe) {
            *w++ = (char)*p;
        } else {
            snprintf(w, 4, "%%%02X", *p);
            w += 3;
        }
    }
    *w = '\0';
    return out;
}

static bool is_http_endpoint(const char *src)
{
    if (!src) {
        return false;
    }
    return (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0);
}

static bool has_mjpeg_ext(const char *name)
{
    if (!name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    return (strcasecmp(dot, ".mjpeg") == 0 ||
            strcasecmp(dot, ".mjpg") == 0 ||
            strcasecmp(dot, ".mjp") == 0);
}

static void free_video_list_locked(void)
{
    if (s_player.videos) {
        for (size_t i = 0; i < s_player.video_count; ++i) {
            free(s_player.videos[i]);
        }
        free(s_player.videos);
        s_player.videos = NULL;
    }
    s_player.video_count = 0;
    s_player.current_index = -1;
    s_player.pending_index = -1;
    s_player.switch_requested = false;
}

static bool copy_video_name_locked(int index, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || index < 0 || !s_player.videos || (size_t)index >= s_player.video_count) {
        return false;
    }
    if (!s_player.videos[index] || s_player.videos[index][0] == '\0') {
        return false;
    }
    snprintf(buf, buf_size, "%s", s_player.videos[index]);
    return true;
}

static bool player_switch_pending(void)
{
    bool pending = false;
    if (player_lock(pdMS_TO_TICKS(20))) {
        pending = s_player.switch_requested;
        player_unlock();
    }
    return pending;
}

bool mjpeg_player_is_active(void)
{
    bool active = true;
    if (player_lock(pdMS_TO_TICKS(20))) {
        active = s_player.active;
        player_unlock();
    }
    return active;
}

void mjpeg_player_set_active(bool active)
{
    if (player_lock(pdMS_TO_TICKS(100))) {
        s_player.active = active;
        player_unlock();
    }
}

static void player_wait_until_active(void)
{
    while (!mjpeg_player_is_active()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void mjpeg_player_stop_for_ws(void)
{
    /* Conservative mode: pause MJPEG decoding without tearing down task internals.
     * This avoids cross-task lifetime races while still releasing decode pressure. */
    mjpeg_player_set_active(false);
}

static esp_err_t player_resolve_target(int *out_index, char *name_buf, size_t name_buf_size)
{
    if (!out_index || !name_buf || name_buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!player_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_player.video_count > 0) {
        int target = s_player.switch_requested ? s_player.pending_index : s_player.current_index;
        if (target < 0 || (size_t)target >= s_player.video_count) {
            target = 0;
        }
        if (copy_video_name_locked(target, name_buf, name_buf_size)) {
            *out_index = target;
            ret = ESP_OK;
        }
    }

    player_unlock();
    return ret;
}

static void player_commit_current_index(int index)
{
    if (!player_lock(pdMS_TO_TICKS(100))) {
        return;
    }
    s_player.current_index = index;
    if (s_player.pending_index == index) {
        s_player.switch_requested = false;
    }
    player_unlock();
}

static size_t choose_cache_capacity(int64_t content_length)
{
    size_t min_bytes = APP_MJPEG_CACHE_MIN_BYTES;
    size_t reserve_bytes = APP_MJPEG_CACHE_RESERVE_BYTES;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (largest <= reserve_bytes) {
        return 0;
    }

    largest -= reserve_bytes;
    size_t target = min_bytes;
    if (content_length > 0 && (uint64_t)content_length > target) {
        target = (size_t)content_length;
    }
    if (APP_MJPEG_CACHE_MAX_BYTES > 0 && target > APP_MJPEG_CACHE_MAX_BYTES) {
        target = APP_MJPEG_CACHE_MAX_BYTES;
    }
    if (target > largest) {
        target = largest;
    }
    if (target < min_bytes) {
        return 0;
    }
    return target;
}

static bool find_next_jpeg_frame(const uint8_t *buf, size_t available, size_t *cursor,
                                 const uint8_t **jpg, size_t *jpg_len)
{
    if (!buf || !cursor || !jpg || !jpg_len || available < 4) {
        return false;
    }

    size_t i = *cursor;
    while (i + 1 < available) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD8) {
            break;
        }
        ++i;
    }
    if (i + 1 >= available) {
        *cursor = (available > 1) ? (available - 1) : available;
        return false;
    }

    size_t start = i;
    i += 2;
    while (i + 1 < available) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD9) {
            *jpg = buf + start;
            *jpg_len = (i + 2) - start;
            *cursor = i + 2;
            return true;
        }
        ++i;
    }

    *cursor = start;
    return false;
}

static bool ensure_output_buffer(mjpeg_ctx_t *ctx, uint32_t width, uint32_t height)
{
    uint32_t aligned_width = ALIGN_UP(width, 16);
    uint32_t aligned_height = ALIGN_UP(height, 16);

    if (ctx->decode_buf && ctx->frame_width == width && ctx->frame_height == height) {
        ctx->present_decode_directly = (width == ctx->display_width &&
                                        height == ctx->display_height &&
                                        aligned_width == width &&
                                        aligned_height == height);
        return true;
    }

    if (ctx->decode_buf) {
        free(ctx->decode_buf);
        ctx->decode_buf = NULL;
        ctx->decode_buf_size = 0;
    }

    size_t decode_need = (size_t)aligned_width * (size_t)aligned_height * 2;
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    size_t allocated = 0;
    ctx->decode_buf = (uint8_t *)jpeg_alloc_decoder_mem(decode_need, &mem_cfg, &allocated);
    if (!ctx->decode_buf) {
        return false;
    }

    ctx->decode_buf_size = allocated;
    ctx->frame_width = width;
    ctx->frame_height = height;
    ctx->aligned_width = aligned_width;
    ctx->aligned_height = aligned_height;
    ctx->present_decode_directly = (width == ctx->display_width &&
                                    height == ctx->display_height &&
                                    aligned_width == width &&
                                    aligned_height == height);
    return true;
}

static bool ensure_present_buffer(mjpeg_ctx_t *ctx)
{
    if (ctx->present_decode_directly) {
        return true;
    }

    size_t need = (size_t)ctx->display_width * (size_t)ctx->display_height * 2;
    if (ctx->frame_buf && ctx->frame_buf_size == need) {
        return true;
    }

    if (ctx->frame_buf) {
        free(ctx->frame_buf);
        ctx->frame_buf = NULL;
        ctx->frame_buf_size = 0;
    }

    ctx->frame_buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->frame_buf) {
        return false;
    }

    ctx->frame_buf_size = need;
    memset(ctx->frame_buf, 0, need);
    return true;
}

static inline uint16_t *src_row_ptr(const mjpeg_ctx_t *ctx, uint32_t y)
{
    return (uint16_t *)(ctx->decode_buf + ((size_t)y * ctx->aligned_width * 2));
}

static void blit_scaled_or_rotated(mjpeg_ctx_t *ctx)
{
    uint32_t src_w = ctx->frame_width;
    uint32_t src_h = ctx->frame_height;
    uint32_t dst_w = ctx->display_width;
    uint32_t dst_h = ctx->display_height;
    bool rotate_90 = ((src_w > src_h) != (dst_w > dst_h));
    uint16_t *dst = (uint16_t *)ctx->frame_buf;

    if (!rotate_90 && src_w == dst_w && src_h == dst_h) {
        size_t row_bytes = (size_t)dst_w * 2;
        for (uint32_t y = 0; y < dst_h; ++y) {
            memcpy((uint8_t *)dst + (size_t)y * row_bytes, src_row_ptr(ctx, y), row_bytes);
        }
        return;
    }

    if (rotate_90 && src_w == dst_h && src_h == dst_w) {
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            uint32_t src_x = dy;
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                uint32_t src_y = src_h - 1 - dx;
                const uint16_t *src_row = src_row_ptr(ctx, src_y);
                dst[(size_t)dy * dst_w + dx] = src_row[src_x];
            }
        }
        return;
    }

    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        uint32_t ry = (uint32_t)(((uint64_t)dy * (rotate_90 ? src_w : src_h)) / dst_h);
        if (ry >= (rotate_90 ? src_w : src_h)) {
            ry = (rotate_90 ? src_w : src_h) - 1;
        }

        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            uint32_t rx = (uint32_t)(((uint64_t)dx * (rotate_90 ? src_h : src_w)) / dst_w);
            if (rx >= (rotate_90 ? src_h : src_w)) {
                rx = (rotate_90 ? src_h : src_w) - 1;
            }

            uint32_t src_x;
            uint32_t src_y;
            if (rotate_90) {
                src_x = ry;
                src_y = src_h - 1 - rx;
            } else {
                src_x = rx;
                src_y = ry;
            }

            const uint16_t *src_row = src_row_ptr(ctx, src_y);
            dst[(size_t)dy * dst_w + dx] = src_row[src_x];
        }
    }
}

static bool decode_and_present_frame(mjpeg_ctx_t *ctx, const uint8_t *jpg, size_t jpg_len,
                                     uint32_t *frame_counter, uint32_t fps_x10, frame_perf_t *perf)
{
    jpeg_decode_picture_info_t info;
    uint32_t vx = 0;
    uint32_t vy = 0;
    uint32_t vw = 0;
    uint32_t vh = 0;

    ui_demo_get_video_viewport(&vx, &vy, &vw, &vh);
    if (vw == 0 || vh == 0) {
        return false;
    }

    ctx->display_width = vw;
    ctx->display_height = vh;

    if (jpeg_decoder_get_info(jpg, (uint32_t)jpg_len, &info) != ESP_OK) {
        return false;
    }
    if (!ensure_output_buffer(ctx, info.width, info.height)) {
        return false;
    }
    if (!ensure_present_buffer(ctx)) {
        return false;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t out_size = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_decoder_process(ctx->decoder, &decode_cfg, jpg, (uint32_t)jpg_len,
                                         ctx->decode_buf, (uint32_t)ctx->decode_buf_size, &out_size);
    int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        return false;
    }

    void *present_buf = ctx->decode_buf;
    if (!ctx->present_decode_directly) {
        blit_scaled_or_rotated(ctx);
        present_buf = ctx->frame_buf;
    }
    int64_t t2 = esp_timer_get_time();

    ui_demo_player_bind_framebuffer(present_buf, ctx->display_width, ctx->display_height);
    int64_t t3 = esp_timer_get_time();

    (*frame_counter)++;
    ui_demo_player_present_frame(*frame_counter, ctx->display_width, ctx->display_height, out_size, fps_x10);

    if (perf) {
        perf->decode_us = t1 - t0;
        perf->copy_us = t2 - t1;
        perf->present_us = t3 - t2;
        perf->out_size = out_size;
        perf->width = info.width;
        perf->height = info.height;
    }
    return true;
}

static void maybe_log_stats(mjpeg_ctx_t *ctx, uint32_t *stat_frames, int64_t *stat_window_start,
                            int64_t *stat_decode_us, int64_t *stat_copy_us, int64_t *stat_present_us)
{
    int64_t now_us = esp_timer_get_time();
    int64_t stat_elapsed = now_us - *stat_window_start;
    if (*stat_frames > 0 && stat_elapsed >= 1000000LL) {
        (void)ctx;
        *stat_window_start = now_us;
        *stat_frames = 0;
        *stat_decode_us = 0;
        *stat_copy_us = 0;
        *stat_present_us = 0;
    }
}

static bool play_one_frame_from_cache(mjpeg_ctx_t *ctx, size_t available_bytes, size_t *play_cursor,
                                      uint32_t *frame_counter, uint32_t *fps_x10, int64_t *last_frame_us,
                                      int64_t *next_deadline, uint32_t *stat_frames,
                                      int64_t *stat_window_start, int64_t *stat_decode_us,
                                      int64_t *stat_copy_us, int64_t *stat_present_us)
{
    const uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    size_t cursor = *play_cursor;
    if (!find_next_jpeg_frame(ctx->cache_buf, available_bytes, &cursor, &jpg, &jpg_len)) {
        return false;
    }

    int64_t frame_start_us = esp_timer_get_time();
    if (*last_frame_us != 0) {
        int64_t delta = frame_start_us - *last_frame_us;
        if (delta > 0) {
            uint32_t instant_fps_x10 = (uint32_t)((10LL * 1000000LL) / delta);
            *fps_x10 = (*fps_x10 == 0) ? instant_fps_x10 : (uint32_t)((*fps_x10 * 7 + instant_fps_x10 * 3) / 10);
        }
    }

    frame_perf_t perf = {0};
    bool ok = decode_and_present_frame(ctx, jpg, jpg_len, frame_counter, *fps_x10, &perf);
    if (!ok) {
        *play_cursor = cursor;
        vTaskDelay(1);
        return true;
    }

    *play_cursor = cursor;
    *last_frame_us = frame_start_us;
    (*stat_frames)++;
    *stat_decode_us += perf.decode_us;
    *stat_copy_us += perf.copy_us;
    *stat_present_us += perf.present_us;
    maybe_log_stats(ctx, stat_frames, stat_window_start, stat_decode_us, stat_copy_us, stat_present_us);

    *next_deadline += 1000000LL / APP_MJPEG_TARGET_FPS;
    int64_t now_us = esp_timer_get_time();
    if (*next_deadline > now_us) {
        vTaskDelay(pdMS_TO_TICKS((*next_deadline - now_us) / 1000));
    } else {
        *next_deadline = now_us;
    }
    return true;
}

static void reset_stream_cache(mjpeg_ctx_t *ctx)
{
    if (ctx->cache_buf) {
        free(ctx->cache_buf);
        ctx->cache_buf = NULL;
    }
    ctx->cache_size = 0;
    ctx->cache_capacity = 0;
    ctx->cache_complete = false;
    ctx->cache_truncated = false;
}

static void cleanup_ctx(mjpeg_ctx_t *ctx)
{
    reset_stream_cache(ctx);
    if (ctx->frame_buf) {
        free(ctx->frame_buf);
    }
    if (ctx->decode_buf) {
        free(ctx->decode_buf);
    }
    if (ctx->decoder) {
        jpeg_del_decoder_engine(ctx->decoder);
    }
}

static esp_err_t http_read_all_text(const char *url, char **out_text)
{
    if (!url || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = APP_MJPEG_HTTP_TIMEOUT_MS,
        .buffer_size = APP_MJPEG_HTTP_READ_CHUNK,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t capacity = (content_length > 0 && content_length < (1024 * 1024)) ? (size_t)content_length + 1 : 4096;
    char *buf = (char *)malloc(capacity);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (1) {
        if (total + APP_MJPEG_HTTP_READ_CHUNK + 1 > capacity) {
            size_t new_capacity = capacity * 2;
            char *new_buf = (char *)realloc(buf, new_capacity);
            if (!new_buf) {
                free(buf);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buf = new_buf;
            capacity = new_capacity;
        }

        int read_len = esp_http_client_read(client, buf + total, (int)(capacity - total - 1));
        if (read_len < 0) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        total += (size_t)read_len;
    }

    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    *out_text = buf;
    return ESP_OK;
}

static esp_err_t select_video_on_server(const char *base_url, const char *filename)
{
    char *encoded = url_encode_component(filename);
    char *url = dup_printf("%s/api/select/%s", base_url, encoded ? encoded : "");
    free(encoded);
    if (!url) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = APP_MJPEG_HTTP_TIMEOUT_MS,
        .buffer_size = APP_MJPEG_HTTP_READ_CHUNK,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    free(url);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            err = ESP_FAIL;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t fetch_video_list_from_server(const char *base_url)
{
    char *url = dup_printf("%s/api/videos", base_url);
    if (!url) {
        return ESP_ERR_NO_MEM;
    }

    char *json_text = NULL;
    esp_err_t err = http_read_all_text(url, &json_text);
    free(url);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json_text);
    free(json_text);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *videos = cJSON_GetObjectItem(root, "videos");
    cJSON *selected = cJSON_GetObjectItem(root, "selected");
    if (!cJSON_IsArray(videos)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int raw_count = cJSON_GetArraySize(videos);
    if (raw_count <= 0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    if (raw_count > PLAYER_MAX_VIDEOS) {
        raw_count = PLAYER_MAX_VIDEOS;
    }

    char **names = (char **)calloc((size_t)raw_count, sizeof(char *));
    if (!names) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    int selected_index = 0;
    int valid_count = 0;
    for (int i = 0; i < raw_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(videos, i);
        if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
            continue;
        }
        names[valid_count] = strdup_safe(item->valuestring);
        if (!names[valid_count]) {
            continue;
        }
        if (selected && cJSON_IsString(selected) && selected->valuestring &&
            strcmp(selected->valuestring, item->valuestring) == 0) {
            selected_index = valid_count;
        }
        valid_count++;
    }
    cJSON_Delete(root);

    if (valid_count <= 0) {
        free(names);
        return ESP_FAIL;
    }

    char **final_names = (char **)realloc(names, (size_t)valid_count * sizeof(char *));
    if (final_names) {
        names = final_names;
    }

    if (!player_lock(pdMS_TO_TICKS(500))) {
        for (int i = 0; i < valid_count; ++i) {
            free(names[i]);
        }
        free(names);
        return ESP_ERR_TIMEOUT;
    }

    free_video_list_locked();
    s_player.videos = names;
    s_player.video_count = (size_t)valid_count;
    s_player.current_index = selected_index;
    s_player.pending_index = selected_index;
    s_player.switch_requested = false;
    player_unlock();
    return ESP_OK;
}

static esp_err_t fetch_video_list_from_sd_dir(const char *dir_path)
{
    if (!dir_path || !dir_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return ESP_FAIL;
    }

    char **names = (char **)calloc(PLAYER_MAX_VIDEOS, sizeof(char *));
    if (!names) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    int valid_count = 0;
    int seen_count = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        seen_count++;
        if (valid_count >= PLAYER_MAX_VIDEOS) {
            break;
        }
        if (ent->d_name[0] == '\0' || ent->d_name[0] == '.') {
            ESP_LOGD(TAG, "sd video skip hidden: %s", ent->d_name);
            continue;
        }
        if (!has_mjpeg_ext(ent->d_name)) {
            ESP_LOGI(TAG, "sd video skip ext: %s (need .mjpeg/.mjpg)", ent->d_name);
            continue;
        }
        names[valid_count] = strdup_safe(ent->d_name);
        if (names[valid_count]) {
            ESP_LOGI(TAG, "sd video add: %s", ent->d_name);
            valid_count++;
        }
    }
    closedir(dir);

    if (valid_count <= 0) {
        ESP_LOGW(TAG, "no playable sd videos in %s, scanned=%d", dir_path, seen_count);
        free(names);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sd video list ready: dir=%s valid=%d scanned=%d", dir_path, valid_count, seen_count);

    if (!player_lock(pdMS_TO_TICKS(500))) {
        for (int i = 0; i < valid_count; ++i) {
            free(names[i]);
        }
        free(names);
        return ESP_ERR_TIMEOUT;
    }

    free_video_list_locked();
    s_player.videos = names;
    s_player.video_count = (size_t)valid_count;
    s_player.current_index = 0;
    s_player.pending_index = 0;
    s_player.switch_requested = false;
    player_unlock();
    return ESP_OK;
}

static esp_err_t open_http_stream(const char *url, esp_http_client_handle_t *out_client, int64_t *out_length)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = APP_MJPEG_HTTP_TIMEOUT_MS,
        .buffer_size = APP_MJPEG_HTTP_READ_CHUNK,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    *out_client = client;
    *out_length = content_length;
    return ESP_OK;
}

static void close_http_stream(esp_http_client_handle_t *client)
{
    if (*client) {
        esp_http_client_close(*client);
        esp_http_client_cleanup(*client);
        *client = NULL;
    }
}

static esp_err_t download_stream_to_cache(mjpeg_ctx_t *ctx, const char *stream_url,
                                          uint32_t *frame_counter, uint32_t *fps_x10,
                                          int64_t *last_frame_us, int64_t *next_deadline,
                                          uint32_t *stat_frames, int64_t *stat_window_start,
                                          int64_t *stat_decode_us, int64_t *stat_copy_us,
                                          int64_t *stat_present_us)
{
    esp_http_client_handle_t client = NULL;
    int64_t content_length = -1;
    esp_err_t err = open_http_stream(stream_url, &client, &content_length);
    if (err != ESP_OK) {
        return err;
    }

    ctx->cache_capacity = choose_cache_capacity(content_length);
    if (ctx->cache_capacity == 0) {
        close_http_stream(&client);
        return ESP_ERR_NO_MEM;
    }

    ctx->cache_buf = (uint8_t *)heap_caps_malloc(ctx->cache_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->cache_buf) {
        close_http_stream(&client);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *rx_buf = (uint8_t *)heap_caps_malloc(APP_MJPEG_HTTP_READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!rx_buf) {
        close_http_stream(&client);
        return ESP_ERR_NO_MEM;
    }

    size_t play_cursor = 0;
    bool playback_started = false;

    while (1) {
        if (!mjpeg_player_is_active() || player_switch_pending()) {
            free(rx_buf);
            close_http_stream(&client);
            return ESP_ERR_INVALID_STATE;
        }

        if (!playback_started && ctx->cache_size >= MIN_VAL(APP_MJPEG_PREFILL_BYTES, ctx->cache_capacity)) {
            playback_started = true;
        }

        if (playback_started) {
            if (play_one_frame_from_cache(ctx, ctx->cache_size, &play_cursor, frame_counter, fps_x10,
                                          last_frame_us, next_deadline, stat_frames,
                                          stat_window_start, stat_decode_us, stat_copy_us,
                                          stat_present_us)) {
                continue;
            }
        } else {
            vTaskDelay(1);
        }

        if (ctx->cache_size >= ctx->cache_capacity) {
            ctx->cache_truncated = true;
            ctx->cache_complete = true;
            break;
        }

        int read_len = esp_http_client_read(client, (char *)rx_buf,
                                            (int)MIN_VAL(APP_MJPEG_HTTP_READ_CHUNK, ctx->cache_capacity - ctx->cache_size));
        if (read_len > 0) {
            memcpy(ctx->cache_buf + ctx->cache_size, rx_buf, (size_t)read_len);
            ctx->cache_size += (size_t)read_len;
            continue;
        }
        if (read_len == 0) {
            ctx->cache_complete = true;
            break;
        }

        free(rx_buf);
        close_http_stream(&client);
        return ESP_FAIL;
    }

    free(rx_buf);
    close_http_stream(&client);
    return (ctx->cache_size > 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t load_local_file_to_cache(mjpeg_ctx_t *ctx, const char *path)
{
    if (!ctx || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_FAIL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long fsz = ftell(fp);
    if (fsz < 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    ctx->cache_capacity = choose_cache_capacity((int64_t)fsz);
    if (ctx->cache_capacity == 0) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    ctx->cache_buf = (uint8_t *)heap_caps_malloc(ctx->cache_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->cache_buf) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    while (ctx->cache_size < ctx->cache_capacity) {
        size_t want = MIN_VAL((size_t)MJPEG_SD_READ_CHUNK, ctx->cache_capacity - ctx->cache_size);
        size_t n = fread(ctx->cache_buf + ctx->cache_size, 1, want, fp);
        if (n == 0) {
            break;
        }
        ctx->cache_size += n;
    }
    fclose(fp);

    if ((size_t)fsz > ctx->cache_capacity) {
        ctx->cache_truncated = true;
    }
    ctx->cache_complete = true;
    ESP_LOGI(TAG, "sd video loaded: path=%s size=%u cached=%u trunc=%d",
             path, (unsigned)fsz, (unsigned)ctx->cache_size, ctx->cache_truncated ? 1 : 0);
    return (ctx->cache_size > 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t stream_local_file_to_cache(mjpeg_ctx_t *ctx, const char *path,
                                            uint32_t *frame_counter, uint32_t *fps_x10,
                                            int64_t *last_frame_us, int64_t *next_deadline,
                                            uint32_t *stat_frames, int64_t *stat_window_start,
                                            int64_t *stat_decode_us, int64_t *stat_copy_us,
                                            int64_t *stat_present_us)
{
    if (!ctx || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_FAIL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long fsz = ftell(fp);
    if (fsz < 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    ctx->cache_capacity = choose_cache_capacity((int64_t)fsz);
    if (ctx->cache_capacity == 0) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    ctx->cache_buf = (uint8_t *)heap_caps_malloc(ctx->cache_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->cache_buf) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t play_cursor = 0;
    bool playback_started = false;
    size_t prefill = MIN_VAL((size_t)MJPEG_SD_PREFILL_BYTES, ctx->cache_capacity);

    while (1) {
        if (!mjpeg_player_is_active() || player_switch_pending()) {
            fclose(fp);
            return ESP_ERR_INVALID_STATE;
        }

        if (!playback_started && ctx->cache_size >= prefill) {
            playback_started = true;
        }

        if (playback_started) {
            if (play_one_frame_from_cache(ctx, ctx->cache_size, &play_cursor, frame_counter, fps_x10,
                                          last_frame_us, next_deadline, stat_frames,
                                          stat_window_start, stat_decode_us, stat_copy_us,
                                          stat_present_us)) {
                continue;
            }
        } else {
            vTaskDelay(1);
        }

        if (ctx->cache_size >= ctx->cache_capacity) {
            ctx->cache_truncated = true;
            ctx->cache_complete = true;
            break;
        }

        size_t want = MIN_VAL((size_t)4096, ctx->cache_capacity - ctx->cache_size);
        size_t n = fread(ctx->cache_buf + ctx->cache_size, 1, want, fp);
        if (n > 0) {
            ctx->cache_size += n;
            continue;
        }
        ctx->cache_complete = true;
        break;
    }

    fclose(fp);
    ESP_LOGI(TAG, "sd video streamed: path=%s size=%u cached=%u prefill=%u",
             path, (unsigned)fsz, (unsigned)ctx->cache_size, (unsigned)prefill);
    return (ctx->cache_size > 0) ? ESP_OK : ESP_FAIL;
}

static void mjpeg_player_task(void *arg)
{
    char *source = (char *)arg;
    mjpeg_ctx_t ctx = {
        .display_width = APP_LCD_H_RES,
        .display_height = APP_LCD_V_RES,
    };
    uint32_t frame_counter = 0;
    uint32_t fps_x10 = 0;
    int64_t last_frame_us = 0;
    int64_t stat_window_start = esp_timer_get_time();
    uint32_t stat_frames = 0;
    int64_t stat_decode_us = 0;
    int64_t stat_copy_us = 0;
    int64_t stat_present_us = 0;

    jpeg_decode_engine_cfg_t engine_cfg = {
        .timeout_ms = 80,
    };
    if (jpeg_new_decoder_engine(&engine_cfg, &ctx.decoder) != ESP_OK) {
        ui_demo_mjpeg_set_status("JPEG decoder init failed");
        free(source);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        player_wait_until_active();

        char target_name[PLAYER_NAME_BUF] = {0};
        int target_index = 0;

        bool need_list = true;
        bool use_sd_source = false;
        char sd_dir[PLAYER_NAME_BUF] = {0};
        if (player_lock(pdMS_TO_TICKS(50))) {
            need_list = (s_player.video_count == 0);
            use_sd_source = s_player.use_sd_source;
            if (use_sd_source && s_player.sd_dir) {
                snprintf(sd_dir, sizeof(sd_dir), "%s", s_player.sd_dir);
            }
            player_unlock();
        }
        if (need_list) {
            esp_err_t list_err = use_sd_source ?
                fetch_video_list_from_sd_dir(sd_dir) :
                fetch_video_list_from_server(source);
            if (list_err != ESP_OK) {
                ui_demo_mjpeg_set_status(use_sd_source ? "No SD videos" : "Failed to fetch video list");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (player_resolve_target(&target_index, target_name, sizeof(target_name)) != ESP_OK) {
            ui_demo_mjpeg_set_status("No videos available");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!use_sd_source) {
            if (select_video_on_server(source, target_name) != ESP_OK) {
                ui_demo_mjpeg_set_status("Failed to select video");
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        player_commit_current_index(target_index);
        ui_demo_mjpeg_set_status(target_name);
        reset_stream_cache(&ctx);
        last_frame_us = 0;
        int64_t next_deadline = esp_timer_get_time();

        esp_err_t err = ESP_FAIL;
        if (use_sd_source) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", sd_dir, target_name);
            err = stream_local_file_to_cache(&ctx, path, &frame_counter, &fps_x10,
                                             &last_frame_us, &next_deadline, &stat_frames,
                                             &stat_window_start, &stat_decode_us, &stat_copy_us,
                                             &stat_present_us);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "load sd video failed: %s (%s)", path, esp_err_to_name(err));
            }
        } else {
            err = download_stream_to_cache(&ctx, s_player.stream_url, &frame_counter, &fps_x10,
                                           &last_frame_us, &next_deadline, &stat_frames,
                                           &stat_window_start, &stat_decode_us, &stat_copy_us,
                                           &stat_present_us);
        }
        if (err == ESP_ERR_INVALID_STATE) {
            reset_stream_cache(&ctx);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (err != ESP_OK || ctx.cache_size == 0) {
            ui_demo_mjpeg_set_status("Failed to cache video");
            reset_stream_cache(&ctx);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        while (mjpeg_player_is_active() && !player_switch_pending()) {
            size_t play_cursor = 0;
            bool any_frame = false;
            while (play_cursor + 1 < ctx.cache_size) {
                if (player_switch_pending()) {
                    break;
                }
                bool advanced = play_one_frame_from_cache(&ctx, ctx.cache_size, &play_cursor, &frame_counter,
                                                          &fps_x10, &last_frame_us, &next_deadline,
                                                          &stat_frames, &stat_window_start,
                                                          &stat_decode_us, &stat_copy_us, &stat_present_us);
                if (!advanced) {
                    break;
                }
                any_frame = true;
            }
            if (!any_frame) {
                if (ctx.cache_size > 4 &&
                    ctx.cache_buf[0] == 0xFF && ctx.cache_buf[1] == 0xD8) {
                    frame_perf_t perf = {0};
                    bool ok = decode_and_present_frame(&ctx, ctx.cache_buf, ctx.cache_size,
                                                       &frame_counter, fps_x10, &perf);
                    if (ok) {
                        any_frame = true;
                        ESP_LOGI(TAG, "sd video fallback single-jpeg rendered: %s", target_name);
                        vTaskDelay(pdMS_TO_TICKS(33));
                    }
                }
            }
            if (!any_frame) {
                ESP_LOGW(TAG, "sd video no decodable jpeg frame: %s", target_name);
                ui_demo_mjpeg_set_status("SD video decode failed");
                break;
            }
            vTaskDelay(1);
        }

        reset_stream_cache(&ctx);
    }

    cleanup_ctx(&ctx);
    free(source);
    vTaskDelete(NULL);
}

esp_err_t mjpeg_player_start(const char *base_url)
{
    if (!base_url) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_player.lock) {
        s_player.lock = xSemaphoreCreateMutex();
        if (!s_player.lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (player_lock(pdMS_TO_TICKS(200))) {
        if (s_player.running) {
            player_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        free(s_player.base_url);
        free(s_player.stream_url);
        free(s_player.sd_dir);
        s_player.base_url = NULL;
        s_player.stream_url = NULL;
        s_player.sd_dir = NULL;

        s_player.use_sd_source = !is_http_endpoint(base_url);
        if (s_player.use_sd_source) {
            s_player.base_url = strdup_safe(base_url);
            s_player.sd_dir = strdup_safe(base_url);
        } else {
            s_player.base_url = strdup_safe(base_url);
            s_player.stream_url = dup_printf("%s/api/mjpeg/stream", base_url);
        }
        s_player.running = true;
        s_player.active = true;
        player_unlock();
    }

    if (!s_player.base_url || (!s_player.use_sd_source && !s_player.stream_url) ||
        (s_player.use_sd_source && !s_player.sd_dir)) {
        return ESP_ERR_NO_MEM;
    }

    char *base_copy = strdup_safe(base_url);
    if (!base_copy) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(mjpeg_player_task, "mjpeg_player", 24 * 1024, base_copy, 5, NULL, 1) != pdPASS) {
        free(base_copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mjpeg_player_switch_relative(int delta)
{
    if (delta == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!player_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_player.videos || s_player.video_count == 0) {
        player_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    int base = s_player.switch_requested ? s_player.pending_index : s_player.current_index;
    if (base < 0 || (size_t)base >= s_player.video_count) {
        base = 0;
    }

    int count = (int)s_player.video_count;
    int next = (base + delta) % count;
    if (next < 0) {
        next += count;
    }

    s_player.pending_index = next;
    s_player.switch_requested = true;

    char name[PLAYER_NAME_BUF] = {0};
    copy_video_name_locked(next, name, sizeof(name));
    player_unlock();

    if (name[0] != '\0') {
        ui_demo_mjpeg_set_status(name);
    }
    return ESP_OK;
}
