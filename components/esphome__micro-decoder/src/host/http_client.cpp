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
#include "platform/memory.h"
#include <curl/curl.h>
#include <strings.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>

namespace micro_decoder {

static constexpr const char* TAG = "micro_decoder.http_client";

static constexpr size_t CURL_BUFFER_SIZE = 32UL * 1024UL;
static constexpr int POLL_TIMEOUT_MS = 100;
static constexpr uint32_t DEFAULT_TIMEOUT_MS = 30000;

static uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

static size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
static size_t curl_header_callback(char* buffer, size_t size, size_t nitems, void* userdata);

// ============================================================================
// CurlHttpClient
// ============================================================================

/// @brief Host HttpClient implementation using libcurl multi API
class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient() = default;

    ~CurlHttpClient() override {
        this->close();
    }

    /// @brief Opens an HTTP connection and blocks until headers arrive
    /// @param url The URL to connect to
    /// @param timeout_ms Connection and transfer timeout in milliseconds; 0 uses a platform default
    /// @param rx_buffer_size Unused on host (curl manages its own buffers)
    /// @return true on success (2xx status), false on connection error or non-2xx status
    bool open(const std::string& url, uint32_t timeout_ms, [[maybe_unused]] size_t rx_buffer_size,
              const std::string& user_agent, const std::string& ca_certificate) override {
        this->close();

        this->easy_ = curl_easy_init();
        if (this->easy_ == nullptr) {
            MD_LOGE(TAG, "curl_easy_init failed");
            return false;
        }

        this->multi_ = curl_multi_init();
        if (this->multi_ == nullptr) {
            curl_easy_cleanup(this->easy_);
            this->easy_ = nullptr;
            MD_LOGE(TAG, "curl_multi_init failed");
            return false;
        }

        if (!this->buf_.allocate(CURL_BUFFER_SIZE)) {
            MD_LOGE(TAG, "Failed to allocate buffer");
            this->close();
            return false;
        }
        this->buf_write_ = 0;
        this->buf_read_ = 0;

        this->headers_ready_ = false;
        this->transfer_done_ = false;
        this->error_ = false;
        this->cancelled_ = false;
        this->response_ = HttpResponse{};

        curl_easy_setopt(this->easy_, CURLOPT_URL, url.c_str());
        if (!user_agent.empty()) {
            curl_easy_setopt(this->easy_, CURLOPT_USERAGENT, user_agent.c_str());
        }
        if (!ca_certificate.empty()) {
#ifdef CURLOPT_CAINFO_BLOB
            // CURL_BLOB_COPY: libcurl copies the data immediately, so .data() lifetime is fine.
            curl_blob blob{
                static_cast<void*>(const_cast<char*>(ca_certificate.data())),
                ca_certificate.size(),
                CURL_BLOB_COPY,
            };
            curl_easy_setopt(this->easy_, CURLOPT_CAINFO_BLOB, &blob);
#else
            MD_LOGE(TAG, "ca_certificate set but CURLOPT_CAINFO_BLOB is unavailable "
                         "(requires libcurl >= 7.77.0); refusing to connect");
            this->close();
            return false;
#endif
        }
        curl_easy_setopt(this->easy_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(this->easy_, CURLOPT_MAXREDIRS, 5L);
        // Cap connect timeout to 10s: connections that take longer almost always fail
        curl_easy_setopt(this->easy_, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(std::min(timeout_ms, static_cast<uint32_t>(10000))));
        // Stall detection: fail if transfer speed stays below 1 byte/s for timeout_ms
        curl_easy_setopt(this->easy_, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(this->easy_, CURLOPT_LOW_SPEED_TIME, static_cast<long>(timeout_ms / 1000));
        curl_easy_setopt(this->easy_, CURLOPT_BUFFERSIZE, static_cast<long>(CURL_BUFFER_SIZE));
        curl_easy_setopt(this->easy_, CURLOPT_WRITEFUNCTION, curl_write_callback);
        curl_easy_setopt(this->easy_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(this->easy_, CURLOPT_HEADERFUNCTION, curl_header_callback);
        curl_easy_setopt(this->easy_, CURLOPT_HEADERDATA, this);

        curl_multi_add_handle(this->multi_, this->easy_);

        // Poll until headers arrive or timeout
        uint64_t deadline = now_ms() + (timeout_ms == 0 ? DEFAULT_TIMEOUT_MS : timeout_ms);
        while (!this->headers_ready_ && !this->transfer_done_) {
            uint64_t now = now_ms();
            if (now >= deadline) {
                MD_LOGE(TAG, "Timeout waiting for headers");
                this->close();
                return false;
            }

            int still_running = 0;
            curl_multi_perform(this->multi_, &still_running);
            this->pump_messages();

            if (!this->headers_ready_ && !this->transfer_done_) {
                int poll_ms = static_cast<int>(
                    std::min(deadline - now, static_cast<uint64_t>(POLL_TIMEOUT_MS)));
                if (poll_ms > 0) {
                    curl_multi_poll(this->multi_, nullptr, 0, poll_ms, nullptr);
                }
            }
        }

        if (this->error_) {
            this->close();
            return false;
        }

        int status = this->response_.status_code;
        static constexpr int HTTP_OK_MIN = 200;
        static constexpr int HTTP_OK_MAX = 300;
        if (status < HTTP_OK_MIN || status >= HTTP_OK_MAX) {
            MD_LOGE(TAG, "HTTP error: %d", status);
            this->close();
            return false;
        }

        MD_LOGD(TAG, "Connected: status=%d content-type='%s'", this->response_.status_code,
                this->response_.content_type.c_str());
        return true;
    }

    /// @brief Returns the HTTP response metadata captured during the header callback
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
        if (!this->buf_) {
            return -1;
        }
        if (this->error_) {
            return -1;
        }

        this->maybe_compact();

        // Deliver already-buffered data
        size_t available = this->buf_write_ - this->buf_read_;
        if (available > 0) {
            size_t to_copy = std::min(available, max_length);
            std::memcpy(buffer, this->buf_.data() + this->buf_read_, to_copy);
            this->buf_read_ += to_copy;
            return static_cast<int>(to_copy);
        }

        // Buffer empty. Drive curl to get more data
        if (!this->transfer_done_) {
            if (this->paused_) {
                curl_easy_pause(this->easy_, CURLPAUSE_CONT);
                this->paused_ = false;
            }
            int still_running = 0;
            curl_multi_perform(this->multi_, &still_running);
            this->pump_messages();

            available = this->buf_write_ - this->buf_read_;
            if (available > 0) {
                size_t to_copy = std::min(available, max_length);
                std::memcpy(buffer, this->buf_.data() + this->buf_read_, to_copy);
                this->buf_read_ += to_copy;
                return static_cast<int>(to_copy);
            }
        }

        return 0;
    }

    /// @brief Returns true when all body data has been received and consumed
    /// @return true when all body data has been received and consumed, false otherwise
    bool is_complete() const override {
        if (!this->buf_) {
            return true;
        }
        return this->transfer_done_ && (this->buf_read_ >= this->buf_write_);
    }

    /// @brief Stops the transfer and frees all curl resources
    void close() override {
        this->cancelled_ = true;

        if (this->multi_ != nullptr && this->easy_ != nullptr) {
            curl_multi_remove_handle(this->multi_, this->easy_);
        }

        if (this->easy_ != nullptr) {
            curl_easy_cleanup(this->easy_);
            this->easy_ = nullptr;
        }

        if (this->multi_ != nullptr) {
            curl_multi_cleanup(this->multi_);
            this->multi_ = nullptr;
        }

        this->buf_ = PlatformBuffer{};
        this->buf_write_ = 0;
        this->buf_read_ = 0;
        this->headers_ready_ = false;
        this->transfer_done_ = false;
        this->error_ = false;
        this->cancelled_ = false;
        this->paused_ = false;
        this->response_ = HttpResponse{};
    }

private:
    friend size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
    friend size_t curl_header_callback(char* buffer, size_t size, size_t nitems, void* userdata);

    /// @brief Drains the curl message queue and updates transfer state
    void pump_messages() {
        int msgs_left = 0;
        CURLMsg* msg = nullptr;
        while ((msg = curl_multi_info_read(this->multi_, &msgs_left)) != nullptr) {
            if (msg->msg == CURLMSG_DONE) {
                CURLcode res = msg->data.result;
                if (res != CURLE_OK && !this->cancelled_) {
                    MD_LOGE(TAG, "curl transfer error: %s", curl_easy_strerror(res));
                    this->error_ = true;
                }
                this->transfer_done_ = true;
                this->headers_ready_ = true;
            }
        }
    }

    /// @brief Compacts the internal buffer by moving unconsumed data to the front
    void maybe_compact() {
        if (this->buf_read_ == 0) {
            return;
        }
        size_t unread = this->buf_write_ - this->buf_read_;
        if (unread > 0) {
            std::memmove(this->buf_.data(), this->buf_.data() + this->buf_read_, unread);
        }
        this->buf_write_ = unread;
        this->buf_read_ = 0;
    }

    // Struct fields
    PlatformBuffer buf_;
    HttpResponse response_;

    // Pointer fields
    CURL* easy_{nullptr};
    CURLM* multi_{nullptr};

    // size_t fields
    size_t buf_read_{0};
    size_t buf_write_{0};

    // 8-bit fields
    bool cancelled_{false};
    bool error_{false};
    bool headers_ready_{false};
    bool paused_{false};
    bool transfer_done_{false};
};

// ============================================================================
// Callbacks
// ============================================================================

static size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* self = static_cast<CurlHttpClient*>(userdata);
    size_t total = size * nmemb;

    if (self->cancelled_) {
        return 0;
    }

    size_t space = self->buf_.size() - self->buf_write_;
    if (space < total) {
        // Not enough room: pause the transfer; read() will unpause after draining
        self->paused_ = true;
        return CURL_WRITEFUNC_PAUSE;
    }

    std::memcpy(self->buf_.data() + self->buf_write_, ptr, total);
    self->buf_write_ += total;
    return total;
}

static size_t curl_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* self = static_cast<CurlHttpClient*>(userdata);
    size_t total = size * nitems;
    // Parse status code from the HTTP status line
    if (total > 5 && std::strncmp(buffer, "HTTP/", 5) == 0) {
        const char* sp = static_cast<const char*>(std::memchr(buffer, ' ', total));
        if (sp != nullptr) {
            ++sp;
            int code = 0;
            auto [ptr, ec] = std::from_chars(sp, buffer + total, code);
            if (ec == std::errc{}) {
                self->response_.status_code = code;
            }
        }
    }

    // Parse Content-Type header
    static constexpr char CT_PREFIX[] = "content-type:";
    static constexpr size_t CT_PREFIX_LEN = sizeof(CT_PREFIX) - 1;
    if (total > CT_PREFIX_LEN && strncasecmp(buffer, CT_PREFIX, CT_PREFIX_LEN) == 0) {
        const char* val = buffer + CT_PREFIX_LEN;
        const char* end = buffer + total;
        while (val < end && (*val == ' ' || *val == '\t')) {
            ++val;
        }
        while (end > val && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) {
            --end;
        }
        if (val < end) {
            self->response_.content_type.assign(val, end);
        }
    }

    // Blank line signals end of headers
    if ((total == 2 && buffer[0] == '\r' && buffer[1] == '\n') ||
        (total == 1 && buffer[0] == '\n')) {
        self->headers_ready_ = true;
    }

    return total;
}

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<HttpClient> create_http_client() {
    return std::make_unique<CurlHttpClient>();
}

}  // namespace micro_decoder
