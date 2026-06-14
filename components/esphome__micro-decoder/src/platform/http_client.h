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

/// @file http_client.h
/// @brief Abstract HTTP client interface used by AudioReader

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace micro_decoder {

/// @brief Metadata returned after a successful HTTP connection
struct HttpResponse {
    int status_code{0};
    std::string content_type;
};

/**
 * @brief Abstract streaming HTTP client
 *
 * Implementations live in src/esp/http_client.cpp and src/host/http_client.cpp.
 *
 * Typical usage:
 *   1. Call open() with a URL and timeout.
 *   2. Call response_info() to inspect the HTTP status and Content-Type.
 *   3. Loop calling read() until is_complete() returns true.
 *   4. Call close() to release resources.
 *
 * @code
 *   client.open(url, timeout_ms);
 *   auto info = client.response_info();
 *   while (!client.is_complete()) {
 *       int n = client.read(buf, sizeof(buf));
 *       if (n < 0) { break; }  // error
 *       if (n == 0) { continue; }  // no data yet, loop until is_complete()
 *   }
 *   client.close();
 * @endcode
 */
class HttpClient {
public:
    virtual ~HttpClient() = default;

    /// @brief Opens the URL and fetches headers
    /// @param url      The URL to connect to
    /// @param timeout_ms  Connection and transfer timeout in milliseconds; 0 uses a platform
    /// default
    /// @param rx_buffer_size  Size of the platform HTTP receive buffer in bytes (ESP-IDF only)
    /// @param user_agent  Optional User-Agent header value; empty string uses the platform default
    /// @param ca_certificate  Optional PEM-encoded CA certificate(s) used to verify HTTPS
    /// servers. Empty string falls back to the platform default trust store (cert bundle on
    /// ESP-IDF, system trust store on host). Ignored for plain HTTP.
    /// @return true on success (2xx or 3xx handled internally)
    virtual bool open(const std::string& url, uint32_t timeout_ms, size_t rx_buffer_size,
                      const std::string& user_agent, const std::string& ca_certificate) = 0;

    /// @brief Returns response metadata (status code, Content-Type header)
    /// @note Valid after a successful open() and before close()
    /// @return Reference to the HttpResponse with the HTTP status code and Content-Type string
    virtual const HttpResponse& response_info() const = 0;

    /// @brief Reads up to max_length bytes of body data into buffer
    /// @param[out] buffer      Destination buffer for received bytes
    /// @param max_length       Maximum number of bytes to read
    /// @return Bytes read (>0), 0 when no data is available (check is_complete() for EOF), -1 on
    /// error
    virtual int read(uint8_t* buffer, size_t max_length) = 0;

    /// @brief Returns true when all body data has been received
    /// @return true when all body data has been received, false otherwise
    virtual bool is_complete() const = 0;

    /// @brief Closes the connection and frees resources
    virtual void close() = 0;
};

/// @brief Creates a platform-specific HttpClient instance
/// @note Defined in src/esp/http_client.cpp or src/host/http_client.cpp.
/// @return Owned HttpClient instance
std::unique_ptr<HttpClient> create_http_client();

}  // namespace micro_decoder
