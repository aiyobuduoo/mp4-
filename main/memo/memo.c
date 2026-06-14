#include "memo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define MEMO_NVS_NAMESPACE "memo_cfg"
#define MEMO_NVS_KEY_LIST  "list"
#define MEMO_NVS_KEY_OLD   "text"
#define MEMO_HTTP_PORT      8080
#define MEMO_LIST_MAGIC     0x4D454D4FUL
#define MEMO_MAX_ITEMS      8
#define MEMO_ITEM_MAX_LEN   128

typedef struct {
    uint32_t magic;
    uint32_t count;
    char items[MEMO_MAX_ITEMS][MEMO_ITEM_MAX_LEN];
} memo_list_t;

static const char *TAG = "memo";
static httpd_handle_t s_memo_httpd;
static SemaphoreHandle_t s_memo_mutex;
static memo_list_t s_memo_list;

static int from_hex(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void url_decode(char *out, size_t out_size, const char *source, size_t source_len)
{
    size_t position = 0;
    for (size_t i = 0; i < source_len && position + 1 < out_size; ++i) {
        if (source[i] == '+') {
            out[position++] = ' ';
        } else if (source[i] == '%' && i + 2 < source_len) {
            int high = from_hex(source[i + 1]);
            int low = from_hex(source[i + 2]);
            if (high >= 0 && low >= 0) {
                out[position++] = (char)((high << 4) | low);
                i += 2;
            } else {
                out[position++] = source[i];
            }
        } else {
            out[position++] = source[i];
        }
    }
    out[position] = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *field = body;
    while (field != NULL && *field != '\0') {
        if (strncmp(field, key, key_len) == 0 && field[key_len] == '=') {
            const char *value = field + key_len + 1;
            const char *end = strchr(value, '&');
            url_decode(out, out_size, value, end != NULL ? (size_t)(end - value) : strlen(value));
            return true;
        }
        field = strchr(field, '&');
        if (field != NULL) {
            ++field;
        }
    }
    return false;
}

static esp_err_t read_request_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= body_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    int received = 0;
    while (received < req->content_len) {
        int result = httpd_req_recv(req, body + received, req->content_len - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }
    body[received] = '\0';
    return ESP_OK;
}

static esp_err_t memo_write_list_locked(nvs_handle_t nvs)
{
    esp_err_t err = nvs_set_blob(nvs, MEMO_NVS_KEY_LIST, &s_memo_list, sizeof(s_memo_list));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    return err;
}

static esp_err_t memo_load_from_nvs(void)
{
    memset(&s_memo_list, 0, sizeof(s_memo_list));
    s_memo_list.magic = MEMO_LIST_MAGIC;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(MEMO_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(s_memo_list);
    err = nvs_get_blob(nvs, MEMO_NVS_KEY_LIST, &s_memo_list, &size);
    if (err == ESP_OK && size == sizeof(s_memo_list) &&
        s_memo_list.magic == MEMO_LIST_MAGIC && s_memo_list.count <= MEMO_MAX_ITEMS) {
        nvs_close(nvs);
        return ESP_OK;
    }

    memset(&s_memo_list, 0, sizeof(s_memo_list));
    s_memo_list.magic = MEMO_LIST_MAGIC;

    char old_text[512] = {0};
    size = sizeof(old_text);
    if (nvs_get_str(nvs, MEMO_NVS_KEY_OLD, old_text, &size) == ESP_OK && old_text[0] != '\0') {
        snprintf(s_memo_list.items[0], sizeof(s_memo_list.items[0]), "%s", old_text);
        s_memo_list.count = 1;
        nvs_erase_key(nvs, MEMO_NVS_KEY_OLD);
    }
    err = memo_write_list_locked(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t memo_add(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_memo_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (s_memo_list.count >= MEMO_MAX_ITEMS) {
        xSemaphoreGive(s_memo_mutex);
        return ESP_ERR_NO_MEM;
    }

    snprintf(s_memo_list.items[s_memo_list.count], MEMO_ITEM_MAX_LEN, "%s", text);
    ++s_memo_list.count;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(MEMO_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = memo_write_list_locked(nvs);
        nvs_close(nvs);
    }
    xSemaphoreGive(s_memo_mutex);
    return err;
}

static esp_err_t memo_delete(size_t index)
{
    if (xSemaphoreTake(s_memo_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (index >= s_memo_list.count) {
        xSemaphoreGive(s_memo_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = index; i + 1 < s_memo_list.count; ++i) {
        memcpy(s_memo_list.items[i], s_memo_list.items[i + 1], MEMO_ITEM_MAX_LEN);
    }
    memset(s_memo_list.items[s_memo_list.count - 1], 0, MEMO_ITEM_MAX_LEN);
    --s_memo_list.count;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(MEMO_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = memo_write_list_locked(nvs);
        nvs_close(nvs);
    }
    xSemaphoreGive(s_memo_mutex);
    return err;
}

static esp_err_t memo_clear(void)
{
    if (xSemaphoreTake(s_memo_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    memset(s_memo_list.items, 0, sizeof(s_memo_list.items));
    s_memo_list.count = 0;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(MEMO_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = memo_write_list_locked(nvs);
        nvs_close(nvs);
    }
    xSemaphoreGive(s_memo_mutex);
    return err;
}

bool memo_get_latest(char *out_text, size_t out_size)
{
    if (out_text == NULL || out_size == 0 || s_memo_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_memo_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    size_t position = 0;
    out_text[0] = '\0';
    for (size_t i = 0; i < s_memo_list.count && position + 1 < out_size; ++i) {
        int written = snprintf(out_text + position, out_size - position,
                               "%s%u. %s", i == 0 ? "" : "\n",
                               (unsigned)(i + 1), s_memo_list.items[i]);
        if (written < 0 || (size_t)written >= out_size - position) {
            break;
        }
        position += (size_t)written;
    }
    xSemaphoreGive(s_memo_mutex);
    return true;
}

static void send_html_escaped(httpd_req_t *req, const char *text)
{
    for (const char *p = text; *p != '\0'; ++p) {
        const char *escaped = NULL;
        if (*p == '&') escaped = "&amp;";
        else if (*p == '<') escaped = "&lt;";
        else if (*p == '>') escaped = "&gt;";
        else if (*p == '"') escaped = "&quot;";
        if (escaped != NULL) {
            httpd_resp_sendstr_chunk(req, escaped);
        } else {
            httpd_resp_send_chunk(req, p, 1);
        }
    }
}

static esp_err_t memo_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Device Memo</title><style>"
        "*{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:Arial,sans-serif;color:#e8ecf3;"
        "background:linear-gradient(145deg,#10131a,#192336);padding:24px 14px}.page{max-width:720px;margin:auto}"
        ".hero{padding:24px;border:1px solid #ffffff18;border-radius:22px;background:#ffffff0c;"
        "box-shadow:0 18px 55px #0006}.eyebrow{color:#7ed7c4;font-size:12px;font-weight:bold;letter-spacing:2px}"
        "h1{margin:7px 0 6px;font-size:30px}.sub{margin:0;color:#9ca9bc;font-size:14px}"
        ".add{display:flex;gap:10px;margin-top:22px;padding:8px;background:#0b1019;border:1px solid #ffffff17;"
        "border-radius:15px}input{min-width:0;flex:1;border:0;outline:0;padding:12px;background:transparent;"
        "color:#fff;font-size:16px}button{border:0;border-radius:11px;padding:11px 17px;font-weight:bold;cursor:pointer}"
        ".primary{color:#07120f;background:linear-gradient(135deg,#62e1c3,#8cf0ac)}"
        ".section{display:flex;align-items:center;justify-content:space-between;margin:28px 3px 12px}"
        ".section h2{font-size:16px;margin:0}.count{padding:5px 9px;border-radius:20px;background:#ffffff12;color:#9ca9bc;font-size:12px}"
        "ol{padding:0;margin:0;list-style:none;display:grid;gap:11px}li{display:flex;align-items:center;gap:13px;"
        "padding:14px;border:1px solid #ffffff15;border-radius:16px;background:#ffffff0b;box-shadow:0 7px 22px #0003}"
        ".num{display:grid;place-items:center;min-width:32px;height:32px;border-radius:10px;background:#65ddc025;"
        "color:#78e5ca;font-weight:bold}.text{flex:1;line-height:1.45;word-break:break-word}li form{margin:0}"
        ".delete{padding:8px 11px;color:#ff8d9c;background:#ff5c7218;border:1px solid #ff71852c}"
        ".empty{padding:30px;text-align:center;color:#7f8da2;border:1px dashed #ffffff20;border-radius:16px}"
        ".footer{margin-top:25px;padding-top:18px;border-top:1px solid #ffffff12;display:flex;justify-content:flex-end}"
        ".clear{color:#ff9bab;background:transparent;border:1px solid #ff75883b}"
        "@media(max-width:520px){body{padding:14px 10px}.hero{padding:19px;border-radius:18px}h1{font-size:25px}"
        ".add{display:block}.primary{width:100%;margin-top:7px}li{align-items:flex-start}.delete{padding:7px 9px}}"
        "</style></head><body><main class='page'><section class='hero'>"
        "<div class='eyebrow'>PERSONAL DASHBOARD</div><h1>Device Memo</h1>"
        "<p class='sub'>Manage the reminders displayed on your device.</p>"
        "<form class='add' method='post' action='/memo'>"
        "<input name='memo' maxlength='127' placeholder='Write a new reminder...' required>"
        "<button class='primary' type='submit'>Add memo</button></form></section>"
        "<div class='section'><h2>Current reminders</h2><span class='count'>Up to 8 items</span></div><ol>");

    if (xSemaphoreTake(s_memo_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_memo_list.count == 0) {
            httpd_resp_sendstr_chunk(req, "<div class='empty'>No reminders yet. Add your first memo above.</div>");
        }
        for (size_t i = 0; i < s_memo_list.count; ++i) {
            char form[320] = {0};
            char number[96] = {0};
            int number_written = snprintf(number, sizeof(number), "<li><span class='num'>%u</span><span class='text'>",
                                          (unsigned)(i + 1));
            if (number_written > 0 && (size_t)number_written < sizeof(number)) {
                httpd_resp_sendstr_chunk(req, number);
            }
            send_html_escaped(req, s_memo_list.items[i]);
            int written = snprintf(form, sizeof(form),
                                   "</span><form method='post' action='/memo/delete'>"
                                   "<input type='hidden' name='id' value='%u'>"
                                   "<button class='delete' type='submit'>Delete</button></form></li>",
                                   (unsigned)i);
            if (written > 0 && (size_t)written < sizeof(form)) {
                httpd_resp_sendstr_chunk(req, form);
            }
        }
        xSemaphoreGive(s_memo_mutex);
    }

    httpd_resp_sendstr_chunk(req,
        "</ol><div class='footer'><form method='post' action='/memo/clear'>"
        "<button class='clear' type='submit'>Clear all reminders</button></form></div></main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t redirect_home(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t memo_add_handler(httpd_req_t *req)
{
    char body[512] = {0};
    char memo[MEMO_ITEM_MAX_LEN] = {0};
    if (read_request_body(req, body, sizeof(body)) != ESP_OK ||
        !form_get_value(body, "memo", memo, sizeof(memo)) || memo[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid memo");
    }

    esp_err_t err = memo_add(memo);
    if (err == ESP_ERR_NO_MEM) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Memo list is full");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }
    return redirect_home(req);
}

static esp_err_t memo_delete_handler(httpd_req_t *req)
{
    char body[64] = {0};
    char id_text[16] = {0};
    if (read_request_body(req, body, sizeof(body)) != ESP_OK ||
        !form_get_value(body, "id", id_text, sizeof(id_text))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid memo id");
    }
    if (memo_delete((size_t)strtoul(id_text, NULL, 10)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Delete failed");
    }
    return redirect_home(req);
}

static esp_err_t memo_clear_handler(httpd_req_t *req)
{
    if (memo_clear() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Clear failed");
    }
    return redirect_home(req);
}

uint16_t memo_service_port(void)
{
    return MEMO_HTTP_PORT;
}

esp_err_t memo_service_start(void)
{
    if (s_memo_httpd != NULL) {
        return ESP_OK;
    }
    if (s_memo_mutex == NULL) {
        s_memo_mutex = xSemaphoreCreateMutex();
        if (s_memo_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = memo_load_from_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot load memo list from NVS: %s", esp_err_to_name(err));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MEMO_HTTP_PORT;
    config.max_uri_handlers = 4;
    err = httpd_start(&s_memo_httpd, &config);
    if (err != ESP_OK) {
        s_memo_httpd = NULL;
        return err;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = memo_page_handler},
        {.uri = "/memo", .method = HTTP_POST, .handler = memo_add_handler},
        {.uri = "/memo/delete", .method = HTTP_POST, .handler = memo_delete_handler},
        {.uri = "/memo/clear", .method = HTTP_POST, .handler = memo_clear_handler},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        err = httpd_register_uri_handler(s_memo_httpd, &handlers[i]);
        if (err != ESP_OK) {
            httpd_stop(s_memo_httpd);
            s_memo_httpd = NULL;
            return err;
        }
    }

    ESP_LOGI(TAG, "Memo list web service started on port %d", MEMO_HTTP_PORT);
    return ESP_OK;
}
