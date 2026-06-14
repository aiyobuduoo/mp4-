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

/// @file ring_buffer.h
/// @brief Byte ring buffer with owned storage, wrapping SpscRingBuffer

#pragma once

#include "platform/memory.h"
#include "platform/spsc_ring_buffer.h"

#include <cstddef>
#include <cstdint>

namespace micro_decoder {

/**
 * @brief Owns storage and wraps SpscRingBuffer with a zero-copy byte-stream API
 *
 * Usage:
 * 1. Call create() with the desired size.
 * 2. Producer calls write(); consumer calls receive_acquire()/receive_release().
 *
 * @code
 * RingBuffer rb;
 * rb.create(1024);
 * rb.write(src, len, 100);
 * @endcode
 */
class RingBuffer {
public:
    RingBuffer() = default;

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /// @brief Allocates storage and initialises the underlying ring buffer
    /// @param size Number of bytes to allocate for the ring buffer
    /// @return true on success, false if allocation fails
    bool create(size_t size);

    /// @brief Writes up to len bytes into the ring buffer
    /// @param data Pointer to the data to write
    /// @param len Number of bytes to write
    /// @param timeout_ms How long to wait if space is unavailable (UINT32_MAX = forever)
    /// @return Number of bytes written; may be less than length if the buffer is full or the
    /// timeout expires
    size_t write(const uint8_t* data, size_t len, uint32_t timeout_ms);

    /// @brief Acquires a zero-copy view of up to max_len contiguous bytes
    /// @note The returned pointer is valid until receive_release() is called.
    /// @note Only one acquisition may be outstanding at a time.
    /// @param[out] data Set to a pointer into ring buffer storage, or nullptr if no data
    /// @param[out] acquired_len Number of contiguous bytes available at *data
    /// @param max_len Maximum number of bytes to acquire
    /// @param timeout_ms How long to wait if no data is available
    void receive_acquire(const uint8_t** data, size_t* acquired_len, size_t max_len,
                         uint32_t timeout_ms);

    /// @brief Releases a previously acquired ring buffer view
    /// @note Must be called exactly once after each successful receive_acquire()
    void receive_release();

    /// @brief Returns the number of bytes available to read
    /// @return Number of bytes available to read
    size_t available() const {
        return this->spsc_.available();
    }

private:
    // Struct fields
    SpscRingBuffer spsc_;
    PlatformBuffer storage_;
};

}  // namespace micro_decoder
