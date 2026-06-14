#include "esp_wifi_remote.h"
#include "esp_log.h"
#include "esp_wifi_types_generic.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define EXAMPLE_ESP_WIFI_SSID ""
#define EXAMPLE_ESP_WIFI_PASS ""
#define EXAMPLE_ESP_MAXIMUM_RETRY 10
#define EXAMPLE_PROV_AP_SSID "SW-ROA-Setup"
#define EXAMPLE_PROV_AP_PASS ""
#define WIFI_NVS_NS "wifi_cfg"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASS "pass"

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;

/* Event bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_PROV_DONE_BIT BIT2

static const char *TAG = "wifi_remote";
static int s_retry_num = 0;
static TaskHandle_t s_reconnect_task = NULL;
static TaskHandle_t s_dns_task = NULL;
static bool s_dns_running = false;
static bool s_sta_flow_enabled = false;
static bool s_wifi_stack_ready = false;
static httpd_handle_t s_portal_httpd = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static wifi_config_t s_prov_wifi_cfg;

static void copy_cstr_to_u8_field(uint8_t *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_len - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void set_ui_status(const char *text)
{
    ESP_LOGI(TAG, "%s", text);
}

static bool wifi_config_has_ssid(const wifi_config_t *cfg)
{
    return cfg && cfg->sta.ssid[0] != '\0';
}

static void wifi_config_set_from_plain(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    copy_cstr_to_u8_field(cfg->sta.ssid, sizeof(cfg->sta.ssid), ssid ? ssid : "");
    copy_cstr_to_u8_field(cfg->sta.password, sizeof(cfg->sta.password), pass ? pass : "");
}

static bool wifi_load_saved_config(wifi_config_t *out_cfg)
{
    if (!out_cfg) {
        return false;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    err = nvs_get_str(nvs, WIFI_NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(nvs);
        return false;
    }
    err = nvs_get_str(nvs, WIFI_NVS_KEY_PASS, pass, &pass_len);
    if (err != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(nvs);

    wifi_config_set_from_plain(out_cfg, ssid, pass);
    return true;
}

static void wifi_save_config(const wifi_config_t *cfg)
{
    if (!wifi_config_has_ssid(cfg)) {
        return;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed when saving wifi: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(nvs, WIFI_NVS_KEY_SSID, (const char *)cfg->sta.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_KEY_PASS, (const char *)cfg->sta.password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save wifi config failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "wifi config saved to NVS, ssid=%s", (const char *)cfg->sta.ssid);
    }
    nvs_close(nvs);
}

static void set_ui_status_retry(int retry, int max_retry)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "Wi-Fi reconnect %d/%d...", retry, max_retry);
    set_ui_status(msg);
}

static int from_hex(char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(10 + c - 'a');
    if (c >= 'A' && c <= 'F') return (int)(10 + c - 'A');
    return -1;
}

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t di = 0;
    for (size_t si = 0; si < src_len && di + 1 < dst_len; ++si) {
        char ch = src[si];
        if (ch == '+') {
            dst[di++] = ' ';
        } else if (ch == '%' && si + 2 < src_len) {
            int hi = from_hex(src[si + 1]);
            int lo = from_hex(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 2;
            } else {
                dst[di++] = ch;
            }
        } else {
            dst[di++] = ch;
        }
    }
    dst[di] = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) {
        return false;
    }
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        bool at_key = (strncmp(p, key, key_len) == 0) && p[key_len] == '=';
        if (at_key) {
            const char *v = p + key_len + 1;
            const char *amp = strchr(v, '&');
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            url_decode(out, out_len, v, vlen);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            ++p;
        }
    }
    return false;
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "portal GET: uri=%s", req->uri ? req->uri : "(null)");
    static const char html[] =
        "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SW-ROA Wi-Fi</title>"
        "<style>body{font-family:sans-serif;margin:24px;line-height:1.6}input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box}"
        "button{padding:10px 16px}small{color:#666}</style></head><body>"
        "<h2>SW-ROA Wi-Fi 配网</h2>"
        "<form method='post' action='/wifi'><label>Wi-Fi 名称(SSID)</label><input name='ssid' maxlength='32' required><label>Wi-Fi 密码</label><input name='password' maxlength='64' type='password'><button type='submit'>保存并连接</button></form>"
        "<p><small>提交后设备会自动切回 STA 并尝试连接。</small></p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_sendstr(req, html);
}

static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "portal redirect: uri=%s", req->uri ? req->uri : "(null)");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t portal_head_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "portal HEAD: uri=%s", req->uri ? req->uri : "(null)");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t portal_favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t portal_post_wifi_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "portal POST: uri=%s len=%d", req->uri ? req->uri : "(null)", req->content_len);
    int total = req->content_len;
    if (total <= 0 || total > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    char body[257];
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, body + off, total - off);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        }
        off += r;
    }
    body[off] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    }
    form_get_value(body, "password", pass, sizeof(pass));

    memset(&s_prov_wifi_cfg, 0, sizeof(s_prov_wifi_cfg));
    copy_cstr_to_u8_field(s_prov_wifi_cfg.sta.ssid, sizeof(s_prov_wifi_cfg.sta.ssid), ssid);
    copy_cstr_to_u8_field(s_prov_wifi_cfg.sta.password, sizeof(s_prov_wifi_cfg.sta.password), pass);

    set_ui_status("Wi-Fi config received, connecting...");
    xEventGroupSetBits(s_wifi_event_group, WIFI_PROV_DONE_BIT);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<html><body><h3>已收到配置，设备正在连接 Wi-Fi...</h3></body></html>");
}


static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    esp_netif_ip_info_t ip_info = {0};
    uint32_t ap_ip = inet_addr("192.168.4.1");
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ap_ip = ip_info.ip.addr;
    }

    uint8_t inbuf[512];
    uint8_t outbuf[512];
    while (s_dns_running) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int r = recvfrom(sock, inbuf, sizeof(inbuf), 0, (struct sockaddr *)&from, &from_len);
        if (r <= 12) {
            continue;
        }

        int qname_end = 12;
        while (qname_end < r && inbuf[qname_end] != 0) {
            qname_end += (int)inbuf[qname_end] + 1;
        }
        if (qname_end + 5 >= r) {
            continue;
        }
        int question_len = (qname_end + 5) - 12;
        int out_len = 12 + question_len + 16;
        if (out_len > (int)sizeof(outbuf)) {
            continue;
        }

        memcpy(outbuf, inbuf, 12 + question_len);
        outbuf[2] = 0x81;
        outbuf[3] = 0x80;
        outbuf[6] = 0x00;
        outbuf[7] = 0x01;
        outbuf[8] = 0x00;
        outbuf[9] = 0x00;
        outbuf[10] = 0x00;
        outbuf[11] = 0x00;

        int ans = 12 + question_len;
        outbuf[ans++] = 0xC0;
        outbuf[ans++] = 0x0C;
        outbuf[ans++] = 0x00;
        outbuf[ans++] = 0x01;
        outbuf[ans++] = 0x00;
        outbuf[ans++] = 0x01;
        outbuf[ans++] = 0x00;
        outbuf[ans++] = 0x00;
        outbuf[ans++] = 0x00;
        outbuf[ans++] = 0x3C;
        outbuf[ans++] = 0x00;
        outbuf[ans++] = 0x04;
        memcpy(&outbuf[ans], &ap_ip, 4);
        ans += 4;

        sendto(sock, outbuf, ans, 0, (struct sockaddr *)&from, from_len);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void stop_dns_server(void)
{
    s_dns_running = false;
    for (int i = 0; i < 30 && s_dns_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t start_provision_http_server(void)
{
    if (s_portal_httpd) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_portal_httpd, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t get_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_get_handler,
    };
    httpd_uri_t get_index = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = portal_get_handler,
    };
    httpd_uri_t head_root = {
        .uri = "/",
        .method = HTTP_HEAD,
        .handler = portal_head_handler,
    };
    httpd_uri_t head_any = {
        .uri = "/*",
        .method = HTTP_HEAD,
        .handler = portal_head_handler,
    };
    httpd_uri_t detect_android_1 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_uri_t detect_android_2 = {
        .uri = "/gen_204",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_uri_t detect_ios = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_uri_t detect_windows_1 = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_uri_t detect_windows_2 = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_uri_t post_wifi = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = portal_post_wifi_handler,
    };
    httpd_uri_t get_favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = portal_favicon_handler,
    };
    httpd_uri_t get_any = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = portal_get_handler,
    };

    const httpd_uri_t *uris[] = {
        &get_root,
        &get_index,
        &head_root,
        &detect_android_1,
        &detect_android_2,
        &detect_ios,
        &detect_windows_1,
        &detect_windows_2,
        &post_wifi,
        &get_favicon,
        &head_any,
        &get_any,
    };
    for (size_t i = 0; i < (sizeof(uris) / sizeof(uris[0])); ++i) {
        esp_err_t re = httpd_register_uri_handler(s_portal_httpd, uris[i]);
        if (re != ESP_OK) {
            ESP_LOGW(TAG, "portal register failed: %s (%s)", uris[i]->uri, esp_err_to_name(re));
        }
    }
    return ESP_OK;
}

static void stop_provision_http_server(void)
{
    if (s_portal_httpd) {
        httpd_stop(s_portal_httpd);
        s_portal_httpd = NULL;
    }
}

static bool start_dns_provisioning(void)
{
    ESP_LOGW(TAG, "start DNS provisioning portal");
    set_ui_status("Wi-Fi failed. Starting DNS provisioning...");
    xEventGroupClearBits(s_wifi_event_group, WIFI_PROV_DONE_BIT);

    s_sta_flow_enabled = false;
    esp_wifi_stop();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = EXAMPLE_PROV_AP_SSID,
            .ssid_len = strlen(EXAMPLE_PROV_AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    if (strlen(EXAMPLE_PROV_AP_PASS) > 0) {
        copy_cstr_to_u8_field(ap_cfg.ap.password, sizeof(ap_cfg.ap.password), EXAMPLE_PROV_AP_PASS);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_ap_netif) {
        esp_netif_ip_info_t ap_info = {0};
        ap_info.ip.addr = inet_addr("192.168.4.1");
        ap_info.gw.addr = inet_addr("192.168.4.1");
        ap_info.netmask.addr = inet_addr("255.255.255.0");

        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ap_info);
        esp_netif_dhcps_start(s_ap_netif);

        esp_netif_ip_info_t now = {0};
        if (esp_netif_get_ip_info(s_ap_netif, &now) == ESP_OK) {
            ESP_LOGI(TAG, "provision AP IP: " IPSTR, IP2STR(&now.ip));
        }
    }

    ESP_LOGI(TAG, "provision AP started, connect SSID=%s and open http://192.168.4.1/",
             EXAMPLE_PROV_AP_SSID);

    if (start_provision_http_server() != ESP_OK) {
        ESP_LOGE(TAG, "provision http server start failed");
        return false;
    }

    if (!s_dns_task) {
        s_dns_running = true;
        if (xTaskCreatePinnedToCore(dns_server_task, "dns_prov", 4096, NULL, 4, &s_dns_task, tskNO_AFFINITY) != pdPASS) {
            s_dns_running = false;
            s_dns_task = NULL;
            stop_provision_http_server();
            return false;
        }
    }

    set_ui_status("DNS配网中: 连SW-ROA-Setup后打开任意网页");
    ESP_LOGI(TAG, "provision portal ready: http://192.168.4.1/");
    return true;
}

static void stop_dns_provisioning(void)
{
    stop_dns_server();
    stop_provision_http_server();
    ESP_LOGI(TAG, "provision portal stopped");
}

static void reconnect_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    esp_wifi_connect();
    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

static void schedule_reconnect(uint32_t delay_ms)
{
    if (!s_sta_flow_enabled) {
        return;
    }
    if (s_reconnect_task) {
        return;
    }
    if (xTaskCreatePinnedToCore(reconnect_task,
                                "wifi_reconn",
                                2048,
                                (void *)(uintptr_t)delay_ms,
                                4,
                                &s_reconnect_task,
                                tskNO_AFFINITY) != pdPASS) {
        s_reconnect_task = NULL;
        esp_wifi_connect();
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        if (s_sta_flow_enabled) {
            schedule_reconnect(300);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (!s_sta_flow_enabled) {
            return;
        }

        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disc ? disc->reason : 0;
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            s_retry_num++;
            uint32_t backoff_ms = 300 + (uint32_t)s_retry_num * 250;
            if (backoff_ms > 2000) {
                backoff_ms = 2000;
            }
            ESP_LOGW(TAG, "disconnected, retry=%d/%d reason=%u backoff=%ums",
                     s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY, (unsigned)reason, (unsigned)backoff_ms);
            set_ui_status_retry(s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
            schedule_reconnect(backoff_ms);
        }
        else
        {
            s_sta_flow_enabled = false;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "connect failed after retries, reason=%u", (unsigned)reason);
            set_ui_status("Wi-Fi failed too many times.");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_sta_flow_enabled = false;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        set_ui_status("Wi-Fi connected.");
    }
}

static bool start_sta_connect_with_config(const wifi_config_t *cfg)
{
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_sta_flow_enabled = true;

    wifi_config_t cfg_local = {0};
    if (cfg) {
        memcpy(&cfg_local, cfg, sizeof(cfg_local));
    }
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg_local));
    ESP_ERROR_CHECK(esp_wifi_start());

    set_ui_status("Connecting to Wi-Fi...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_init_sta(void)
{
    if (!s_wifi_stack_ready) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            return false;
        }

        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t loop_err = esp_event_loop_create_default();
        if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(loop_err);
        }

        s_sta_netif = esp_netif_create_default_wifi_sta();
        s_ap_netif = esp_netif_create_default_wifi_ap();
        (void)s_sta_netif;

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_got_ip));

        s_wifi_stack_ready = true;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
        },
    };
    wifi_config_t saved_cfg = {0};
    if (wifi_load_saved_config(&saved_cfg) && wifi_config_has_ssid(&saved_cfg)) {
        memcpy(&wifi_config, &saved_cfg, sizeof(wifi_config));
        ESP_LOGI(TAG, "using saved wifi config, ssid=%s", (const char *)wifi_config.sta.ssid);
    } else {
        ESP_LOGI(TAG, "using default wifi config, ssid=%s", EXAMPLE_ESP_WIFI_SSID);
    }

    for (;;) {
        if (start_sta_connect_with_config(&wifi_config)) {
            ESP_LOGI(TAG, "connected to ap SSID:%s", (const char *)wifi_config.sta.ssid);
            wifi_save_config(&wifi_config);
            stop_dns_provisioning();
            return true;
        }

        ESP_LOGW(TAG, "connect failed after retry limit, entering DNS provisioning");
        if (!start_dns_provisioning()) {
            ESP_LOGE(TAG, "failed to start DNS provisioning");
            return false;
        }

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_PROV_DONE_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               portMAX_DELAY);
        if (!(bits & WIFI_PROV_DONE_BIT)) {
            ESP_LOGE(TAG, "provisioning ended unexpectedly");
            return false;
        }

        stop_dns_provisioning();
        memcpy(&wifi_config, &s_prov_wifi_cfg, sizeof(wifi_config));
        set_ui_status("Wi-Fi provisioning done, retrying...");
    }
}

bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_get_ip_address(char *out_ip, size_t out_size)
{
    if (out_ip == NULL || out_size == 0 || s_sta_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    int written = snprintf(out_ip, out_size, IPSTR, IP2STR(&ip_info.ip));
    return written > 0 && (size_t)written < out_size;
}


