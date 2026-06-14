// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "platform/http_client.h"

#include "platform/logging.h"
#include <esp_http_client.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

#include <cstring>
#include <string>

namespace micro_decoder {

static constexpr const char* TAG = "micro_decoder.http_client";

static constexpr uint8_t MAX_REDIRECTIONS = 5;
static constexpr uint8_t MAX_HEADER_ATTEMPTS = 6;

/// @brief Returns true if the URL begins with an "https:" scheme (case-insensitive)
static bool url_has_https_scheme(const std::string& url) {
    static constexpr char SCHEME[] = "https:";
    static constexpr size_t SCHEME_LEN = sizeof(SCHEME) - 1;
    if (url.size() < SCHEME_LEN) {
        return false;
    }
    for (size_t i = 0; i < SCHEME_LEN; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        if (c != SCHEME[i]) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// EspHttpClient
// ============================================================================

/// @brief ESP-IDF HttpClient implementation using esp_http_client
class EspHttpClient final : public HttpClient {
public:
    EspHttpClient() = default;

    ~EspHttpClient() override {
        this->close();
    }

    /// @brief Opens an HTTP connection and begins streaming via esp_http_client
    /// @param url The URL to connect to
    /// @param timeout_ms Connection and transfer timeout in milliseconds; 0 uses a platform default
    /// @param rx_buffer_size Size of the ESP-IDF HTTP receive buffer in bytes
    /// @return true on success (2xx status), false on connection error or non-2xx status
    bool open(const std::string& url, uint32_t timeout_ms, size_t rx_buffer_size,
              const std::string& user_agent, const std::string& ca_certificate) override {
        this->close();
        this->complete_ = false;
        this->response_ = HttpResponse{};

        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.disable_auto_redirect = false;
        cfg.max_redirection_count = MAX_REDIRECTIONS;
        cfg.event_handler = http_event_handler;
        cfg.user_data = this;
        cfg.buffer_size = static_cast<int>(rx_buffer_size);
        cfg.keep_alive_enable = true;
        cfg.timeout_ms = static_cast<int>(timeout_ms);
        if (!user_agent.empty()) {
            cfg.user_agent = user_agent.c_str();
        }

        if (url_has_https_scheme(url)) {
            if (!ca_certificate.empty()) {
                cfg.cert_pem = ca_certificate.c_str();
            } else {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
                cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
            }
        }

        this->client_ = esp_http_client_init(&cfg);
        if (this->client_ == nullptr) {
            MD_LOGE(TAG, "esp_http_client_init failed");
            return false;
        }

        esp_err_t err = esp_http_client_open(this->client_, 0);
        if (err != ESP_OK) {
            MD_LOGE(TAG, "Failed to open URL: %s", esp_err_to_name(err));
            this->cleanup();
            return false;
        }

        int64_t header_len = esp_http_client_fetch_headers(this->client_);
        uint8_t attempts = 0;
        while (header_len < 0 && attempts < MAX_HEADER_ATTEMPTS) {
            if (header_len != -ESP_ERR_HTTP_EAGAIN) {
                MD_LOGE(TAG, "Failed to fetch headers");
                this->cleanup();
                return false;
            }
            this->cleanup();
            this->response_ = HttpResponse{};
            // cfg is unchanged across retries; cert_pem / crt_bundle_attach persist.
            this->client_ = esp_http_client_init(&cfg);
            if (this->client_ == nullptr) {
                MD_LOGE(TAG, "esp_http_client_init failed in retry loop");
                return false;
            }
            esp_err_t retry_err = esp_http_client_open(this->client_, 0);
            if (retry_err != ESP_OK) {
                MD_LOGE(TAG, "Failed to open URL in retry: %s", esp_err_to_name(retry_err));
                this->cleanup();
                return false;
            }
            header_len = esp_http_client_fetch_headers(this->client_);
            ++attempts;
        }

        if (header_len < 0) {
            MD_LOGE(TAG, "Failed to fetch headers after %u attempts", attempts);
            this->cleanup();
            return false;
        }

        int status = esp_http_client_get_status_code(this->client_);

        // Follow redirects manually. Note cfg.disable_auto_redirect only applies to
        // esp_http_client_perform(), not the open/fetch_headers path used here.
        uint8_t redirect_count = 0;
        while (esp_http_client_set_redirection(this->client_) == ESP_OK &&
               redirect_count < MAX_REDIRECTIONS) {
            err = esp_http_client_open(this->client_, 0);
            if (err != ESP_OK) {
                this->cleanup();
                return false;
            }
            header_len = esp_http_client_fetch_headers(this->client_);
            if (header_len < 0) {
                this->cleanup();
                return false;
            }
            status = esp_http_client_get_status_code(this->client_);
            ++redirect_count;
        }

        static constexpr int HTTP_OK_MIN = 200;
        static constexpr int HTTP_OK_MAX = 300;
        if (status < HTTP_OK_MIN || status >= HTTP_OK_MAX) {
            MD_LOGE(TAG, "HTTP error: %d", status);
            this->cleanup();
            return false;
        }

        this->response_.status_code = status;
        MD_LOGD(TAG, "Connected: status=%d content-type='%s'", status,
                this->response_.content_type.c_str());
        return true;
    }

    /// @brief Returns the HTTP response metadata captured during the event callback
    /// @note Valid after a successful open()
    /// @return HttpResponse with the HTTP status code and Content-Type string
    const HttpResponse& response_info() const override {
        return this->response_;
    }

    /// @brief Reads the next chunk of response body data
    /// @param[out] buffer Destination buffer for received bytes
    /// @param max_length Maximum number of bytes to read
    /// @return Bytes read (>0), 0 when no data is available (check is_complete() for EOF), -1 on
    /// error
    int read(uint8_t* buffer, size_t max_length) override {
        if (this->client_ == nullptr) {
            return -1;
        }
        if (this->complete_) {
            return 0;
        }
        int received = esp_http_client_read(this->client_, reinterpret_cast<char*>(buffer),
                                            static_cast<int>(max_length));
        if (received < 0) {
            if (received == -ESP_ERR_HTTP_EAGAIN) {
                return 0;  // Timeout, retry
            }
            return -1;
        }
        if (received == 0 && esp_http_client_is_complete_data_received(this->client_)) {
            this->complete_ = true;
        }
        return received;
    }

    /// @brief Returns true when the HTTP response body has been fully received
    /// @return true when all body data has been received, false otherwise
    bool is_complete() const override {
        return this->complete_;
    }

    /// @brief Closes the HTTP connection and frees esp_http_client resources
    /// @note Safe to call multiple times; a no-op if already closed
    void close() override {
        this->cleanup();
    }

private:
    /// @brief Handles HTTP client events from esp_http_client
    static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
        auto* self = static_cast<EspHttpClient*>(evt->user_data);
        if (evt->event_id == HTTP_EVENT_ON_HEADER) {
            if (strcasecmp(evt->header_key, "Content-Type") == 0) {
                self->response_.content_type = evt->header_value;
            }
        }
        return ESP_OK;
    }

    /// @brief Closes and cleans up the HTTP client handle
    void cleanup() {
        if (this->client_ != nullptr) {
            esp_http_client_close(this->client_);
            esp_http_client_cleanup(this->client_);
            this->client_ = nullptr;
        }
    }

    // Struct fields
    HttpResponse response_;

    // Pointer fields
    esp_http_client_handle_t client_{nullptr};

    // bool fields
    bool complete_{false};
};

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<HttpClient> create_http_client() {
    return std::make_unique<EspHttpClient>();
}

}  // namespace micro_decoder
