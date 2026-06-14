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

/// @file audio_reader.h
/// @brief AudioReader: streams HTTP audio data into a RingBuffer

#pragma once

#include "md_transfer_buffer.h"
#include "micro_decoder/types.h"
#include "platform/http_client.h"
#include "ring_buffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace micro_decoder {

/// @brief State returned by AudioReader::run()
enum class AudioReaderState : uint8_t {
    READING = 0,
    IDLE,
    FINISHED,
    FAILED,
};

/**
 * @brief Reads raw audio data from an HTTP URL into a RingBuffer.
 *
 * Runs on the reader thread. Typical usage:
 *   1. Construct with a TransferBuffer size and HTTP timeout.
 *   2. Call set_sink() to set the destination RingBuffer.
 *   3. Call start_url() to connect and detect the file type.
 *   4. Call run() in a loop until it returns FINISHED or FAILED.
 *
 * @code
 * RingBuffer rb;
 * rb.create(65536);
 * AudioReader reader(4096, 5000, 20, 2048, "my-agent/1.0", "");
 * reader.set_sink(&rb);
 * if (reader.start_url("http://example.com/song.flac")) {
 *     AudioFileType type = reader.file_type();
 *     while (reader.run() == AudioReaderState::READING) {}
 * }
 * @endcode
 */
class AudioReader {
public:
    explicit AudioReader(size_t transfer_buffer_size, uint32_t http_timeout_ms,
                         uint32_t write_timeout_ms, size_t http_rx_buffer_size,
                         std::string user_agent, std::string ca_certificate);
    ~AudioReader();

    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;

    /// @brief Returns the detected file type (valid after a successful start_url())
    /// @return Detected audio file type
    AudioFileType file_type() const {
        return this->file_type_;
    }

    /// @brief Sets the ring buffer where raw audio data is written
    /// @param ring_buffer Destination ring buffer for raw audio data
    void set_sink(RingBuffer* ring_buffer) {
        this->ring_buffer_ = ring_buffer;
    }

    /// @brief Starts reading from an HTTP URL. Determines the file type from
    /// Content-Type header or URL extension
    /// @param url HTTP or HTTPS URL to read from
    /// @return true on successful connection
    bool start_url(const std::string& url);

    /// @brief Reads a chunk of data and forwards it to the ring buffer
    /// @note Call in a loop until the return value is not READING.
    /// @return Current reader state after this iteration
    AudioReaderState run();

private:
    // Struct fields
    TransferBuffer transfer_buffer_;
    std::string user_agent_;
    std::string ca_certificate_;

    // Pointer fields
    std::unique_ptr<HttpClient> client_;
    RingBuffer* ring_buffer_{nullptr};

    // size_t fields
    size_t http_rx_buffer_size_;

    // 32-bit fields
    uint32_t http_timeout_ms_;
    uint32_t write_timeout_ms_;

    // 8-bit fields
    AudioFileType file_type_{AudioFileType::NONE};

    // bool fields
    bool allocation_ok_{false};
};

}  // namespace micro_decoder
