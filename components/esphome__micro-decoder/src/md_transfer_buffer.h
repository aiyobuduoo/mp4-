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

/// @file md_transfer_buffer.h
/// @brief Flat byte buffer with start pointer and length cursors for staging data
/// between HttpClient/RingBuffer and AudioDecoder

#pragma once

#include "platform/memory.h"

#include <cstddef>
#include <cstdint>

namespace micro_decoder {

/**
 * @brief Flat byte buffer with start-pointer and length cursors
 *
 * Backed by a PlatformBuffer. Provides a data_start_ pointer and buffer_length_
 * counter that track the usable window within the allocation.
 *
 * 1. Call allocate() to reserve memory.
 * 2. Write data into the region from get_buffer_end() up to free() bytes.
 * 3. Call increase_length() to commit written bytes.
 * 4. Read data from get_buffer_start() up to available() bytes.
 * 5. Call decrease_length() to consume read bytes.
 *
 * @code
 * TransferBuffer buf;
 * buf.allocate(1024);
 * memcpy(buf.get_buffer_end(), source, n);
 * buf.increase_length(n);
 * process(buf.get_buffer_start(), buf.available());
 * buf.decrease_length(consumed);
 * @endcode
 */
class TransferBuffer {
public:
    TransferBuffer() = default;

    TransferBuffer(const TransferBuffer&) = delete;
    TransferBuffer& operator=(const TransferBuffer&) = delete;

    /// @brief Allocates the underlying buffer
    /// @param size Number of bytes to allocate
    /// @return true if allocation succeeded.
    bool allocate(size_t size);

    /// @brief Reallocates the underlying buffer to a new size, preserving existing data
    /// @param size New size in bytes
    /// @return true if reallocation succeeded.
    /// @note On failure, the original buffer remains valid.
    bool reallocate(size_t size);

    /// @brief Number of bytes of valid data currently in the buffer
    /// @return Number of readable bytes
    size_t available() const {
        return this->buffer_length_;
    }

    /// @brief Total allocation size in bytes
    /// @return Total buffer capacity in bytes
    size_t capacity() const {
        return this->buffer_.size();
    }

    /// @brief Number of bytes available to write at get_buffer_end()
    /// @return Number of writable bytes
    size_t free() const {
        size_t used_offset = static_cast<size_t>(this->data_start_ - this->buffer_.data());
        return this->buffer_.size() - used_offset - this->buffer_length_;
    }

    /// @brief Returns a pointer one past the end of available data; new data is written here
    /// @return Pointer to the first writable byte
    uint8_t* get_buffer_end() {
        return this->data_start_ + this->buffer_length_;
    }

    /// @brief Returns a pointer to the start of available (unconsumed) data
    /// @return Pointer to the first readable byte
    uint8_t* get_buffer_start() {
        return this->data_start_;
    }

    /// @brief Mark bytes as consumed from the front
    /// @param bytes Number of bytes to consume
    void decrease_length(size_t bytes);

    /// @brief Mark bytes as written at the end
    /// @param bytes Number of bytes to commit
    void increase_length(size_t bytes);

private:
    // Struct fields
    PlatformBuffer buffer_;

    // Pointer fields
    uint8_t* data_start_{nullptr};

    // size_t fields
    size_t buffer_length_{0};
};

}  // namespace micro_decoder
