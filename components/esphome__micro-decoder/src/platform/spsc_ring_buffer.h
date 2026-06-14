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

/// @file spsc_ring_buffer.h
/// @brief Platform-abstracted single-producer/single-consumer byte ring buffer backed by a
/// FreeRTOS BYTEBUF ring buffer on ESP and a mutex/condition-variable implementation on host

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef ESP_PLATFORM

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include <algorithm>

namespace micro_decoder {

/**
 * @brief Single-producer/single-consumer byte ring buffer with caller-provided storage
 *
 * Backed by a FreeRTOS BYTEBUF ring buffer on ESP. Supports partial reads and writes.
 *
 * Usage:
 * 1. Allocate storage externally (e.g., via PlatformBuffer)
 * 2. Call create() with the storage pointer and size
 * 3. Use write() from the producer thread and read() from the consumer thread
 *
 * @code
 * uint8_t storage[4096];
 * SpscRingBuffer rb;
 * rb.create(storage, sizeof(storage));
 * rb.write(data, len, timeout_ms);
 * @endcode
 */
class SpscRingBuffer {
public:
    SpscRingBuffer() = default;
    ~SpscRingBuffer() {
        if (this->handle_ != nullptr) {
            vRingbufferDelete(this->handle_);
        }
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /// @brief Creates the ring buffer with caller-provided storage
    /// @note Safe to call again to reset an existing ring buffer
    /// @param size Size of the storage buffer in bytes
    /// @param storage Pointer to caller-provided backing storage
    /// @return true if creation succeeded
    bool create(size_t size, uint8_t* storage) {
        if (this->handle_ != nullptr) {
            vRingbufferDelete(this->handle_);
            this->handle_ = nullptr;
        }
        this->storage_size_ = size;
        this->handle_ =
            xRingbufferCreateStatic(size, RINGBUF_TYPE_BYTEBUF, storage, &this->structure_);
        return this->handle_ != nullptr;
    }

    /// @brief Writes up to len bytes into the ring buffer
    /// Waits up to timeout_ms for space, then writes as much as possible.
    /// @param data Pointer to the source data
    /// @param len Number of bytes to write
    /// @param timeout_ms Maximum time to wait for space in milliseconds
    /// @return Number of bytes actually written
    size_t write(const void* data, size_t len, uint32_t timeout_ms) {
        // Try the full write first
        if (xRingbufferSend(this->handle_, data, len, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            return len;
        }
        // Full write did not fit. Try a partial write with whatever space is available
        size_t used = this->available();
        size_t free_space = (this->storage_size_ > used) ? this->storage_size_ - used : 0;
        if (free_space == 0) {
            return 0;
        }
        size_t to_write = std::min(len, free_space);
        if (xRingbufferSend(this->handle_, data, to_write, 0) == pdTRUE) {
            return to_write;
        }
        return 0;
    }

    /// @brief Acquires a zero-copy view of up to max_len bytes from the ring buffer
    /// The returned pointer points directly into ring buffer storage and is valid until
    /// receive_release() is called. Only one acquisition may be outstanding at a time.
    /// @param[out] data Set to a pointer into ring buffer storage, or nullptr if no data
    /// @param[out] acquired_len Number of contiguous bytes available at *data
    /// @param max_len Maximum number of bytes to acquire
    /// @param timeout_ms Time to wait for data in milliseconds
    void receive_acquire(const uint8_t** data, size_t* acquired_len, size_t max_len,
                         uint32_t timeout_ms) {
        size_t item_size = 0;
        void* item =
            xRingbufferReceiveUpTo(this->handle_, &item_size, pdMS_TO_TICKS(timeout_ms), max_len);
        if (item == nullptr) {
            *data = nullptr;
            *acquired_len = 0;
            return;
        }
        this->acquired_item_ = item;
        *data = static_cast<const uint8_t*>(item);
        *acquired_len = item_size;
    }

    /// @brief Releases a previously acquired ring buffer item
    /// Must be called exactly once after each successful receive_acquire()
    void receive_release() {
        if (this->acquired_item_ != nullptr) {
            vRingbufferReturnItem(this->handle_, this->acquired_item_);
            this->acquired_item_ = nullptr;
        }
    }

    /// @brief Returns the number of bytes available to read
    /// @return Number of bytes available to read
    size_t available() const {
        UBaseType_t waiting = 0;
        vRingbufferGetInfo(this->handle_, nullptr, nullptr, nullptr, nullptr, &waiting);
        return static_cast<size_t>(waiting);
    }

private:
    // Struct fields
    StaticRingbuffer_t structure_;

    // Pointer fields
    void* acquired_item_{nullptr};
    RingbufHandle_t handle_{nullptr};

    // size_t fields
    size_t storage_size_{0};
};

}  // namespace micro_decoder

#else  // Host

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace micro_decoder {

/**
 * @brief Single-producer/single-consumer byte ring buffer with caller-provided storage
 *
 * Backed by a mutex/condition-variable pair. Supports partial reads and writes.
 *
 * Usage:
 * 1. Allocate storage externally (e.g., via PlatformBuffer)
 * 2. Call create() with the storage pointer and size
 * 3. Use write() from the producer thread and read() from the consumer thread
 *
 * @code
 * uint8_t storage[4096];
 * SpscRingBuffer rb;
 * rb.create(storage, sizeof(storage));
 * rb.write(data, len, timeout_ms);
 * @endcode
 */
class SpscRingBuffer {
public:
    SpscRingBuffer() = default;
    ~SpscRingBuffer() = default;

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /// @brief Creates the ring buffer with caller-provided storage
    /// @note Safe to call again to reset an existing ring buffer
    /// @param size Size of the storage buffer in bytes
    /// @param storage Pointer to caller-provided backing storage
    /// @return true if creation succeeded
    bool create(size_t size, uint8_t* storage) {
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->storage_ = storage;
        this->storage_size_ = size;
        this->write_offset_ = 0;
        this->read_offset_ = 0;
        this->free_bytes_ = size;
        this->acquired_len_ = 0;
        return true;
    }

    /// @brief Writes up to len bytes into the ring buffer
    /// Waits up to timeout_ms for any free space, then writes as much as possible.
    /// @param data Pointer to the source data
    /// @param len Number of bytes to write
    /// @param timeout_ms Maximum time to wait for space in milliseconds
    /// @return Number of bytes actually written
    size_t write(const void* data, size_t len, uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lock(this->mtx_);

        auto has_space = [this] { return this->free_bytes_ > 0; };
        if (!has_space()) {
            if (!wait_cv(lock, this->cv_write_, has_space, timeout_ms)) {
                return 0;
            }
        }

        size_t to_write = std::min(len, this->free_bytes_);
        circular_copy_in(static_cast<const uint8_t*>(data), this->write_offset_, to_write);

        this->write_offset_ = (this->write_offset_ + to_write) % this->storage_size_;
        this->free_bytes_ -= to_write;

        this->cv_read_.notify_all();
        return to_write;
    }

    /// @brief Acquires a zero-copy view of up to max_len contiguous bytes from the ring buffer
    /// The returned pointer points directly into ring buffer storage and is valid until
    /// receive_release() is called. Only one acquisition may be outstanding at a time.
    /// May return fewer bytes than available if data wraps around the buffer boundary.
    /// @param[out] data Set to a pointer into ring buffer storage, or nullptr if no data
    /// @param[out] acquired_len Number of contiguous bytes available at *data
    /// @param max_len Maximum number of bytes to acquire
    /// @param timeout_ms Time to wait for data in milliseconds
    void receive_acquire(const uint8_t** data, size_t* acquired_len, size_t max_len,
                         uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lock(this->mtx_);
        auto has_data = [this] { return this->free_bytes_ < this->storage_size_; };
        if (!has_data()) {
            if (!wait_cv(lock, this->cv_read_, has_data, timeout_ms)) {
                *data = nullptr;
                *acquired_len = 0;
                return;
            }
        }
        size_t available = this->storage_size_ - this->free_bytes_;
        size_t contiguous = this->storage_size_ - this->read_offset_;
        size_t to_acquire = std::min({max_len, available, contiguous});
        *data = this->storage_ + this->read_offset_;
        *acquired_len = to_acquire;
        this->acquired_len_ = to_acquire;
    }

    /// @brief Releases a previously acquired ring buffer item
    /// Must be called exactly once after each successful receive_acquire()
    void receive_release() {
        std::lock_guard<std::mutex> lock(this->mtx_);
        if (this->acquired_len_ == 0) {
            return;
        }
        this->read_offset_ = (this->read_offset_ + this->acquired_len_) % this->storage_size_;
        this->free_bytes_ += this->acquired_len_;
        this->acquired_len_ = 0;
        this->cv_write_.notify_all();
    }

    /// @brief Returns the number of bytes available to read
    /// @return Number of bytes available to read
    size_t available() const {
        std::lock_guard<std::mutex> lock(this->mtx_);
        return this->storage_size_ - this->free_bytes_;
    }

private:
    /// @brief Copies data into the ring buffer storage at the given circular offset
    /// @param src Source buffer to copy from
    /// @param offset Byte offset into the ring buffer storage where copying starts
    /// @param len Number of bytes to copy
    void circular_copy_in(const uint8_t* src, size_t offset, size_t len) {
        size_t first = std::min(len, this->storage_size_ - offset);
        std::memcpy(this->storage_ + offset, src, first);
        if (first < len) {
            std::memcpy(this->storage_, src + first, len - first);
        }
    }

    /// @brief Waits on a condition variable with an optional timeout
    /// @param[in,out] lock Unique lock held by the caller; temporarily released while waiting
    /// @param cv Condition variable to wait on
    /// @param pred Predicate that must become true for the wait to complete
    /// @param timeout_ms Maximum time to wait in milliseconds; UINT32_MAX waits indefinitely
    /// @return true if the predicate was satisfied before the timeout, false if timed out
    template <typename Predicate>
    bool wait_cv(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, Predicate pred,
                 uint32_t timeout_ms) {
        if (timeout_ms == 0) {
            return false;
        }
        if (timeout_ms == UINT32_MAX) {
            cv.wait(lock, pred);
            return true;
        }
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    }

    // Struct fields
    std::condition_variable cv_read_;
    std::condition_variable cv_write_;
    mutable std::mutex mtx_;

    // Pointer fields
    uint8_t* storage_{nullptr};

    // size_t fields
    size_t acquired_len_{0};
    size_t free_bytes_{0};
    size_t read_offset_{0};
    size_t storage_size_{0};
    size_t write_offset_{0};
};

}  // namespace micro_decoder

#endif  // ESP_PLATFORM
