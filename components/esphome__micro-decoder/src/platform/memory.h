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

/// @file memory.h
/// @brief Platform-abstracted memory allocation preferring SPIRAM on ESP, plus RAII buffer helper

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#ifdef ESP_PLATFORM

#include <esp_heap_caps.h>

namespace micro_decoder {

/// @brief Allocates memory, preferring SPIRAM on ESP-IDF. Falls back to internal RAM
/// @param size Number of bytes to allocate
/// @return Pointer to allocated memory, or nullptr on failure
inline void* platform_malloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL);
}

/// @brief Reallocates memory, preferring SPIRAM on ESP-IDF. Falls back to internal RAM
/// @param ptr Previously allocated memory block
/// @param size New size in bytes
/// @return Pointer to allocated memory, or nullptr on failure
inline void* platform_realloc(void* ptr, size_t size) {
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL);
}

/// @brief Frees memory allocated by platform_malloc or platform_realloc
/// @param ptr Memory to free
inline void platform_free(void* ptr) {
    heap_caps_free(ptr);
}

}  // namespace micro_decoder

#else  // Host

namespace micro_decoder {

/// @brief Allocates memory using the standard allocator
/// @param size Number of bytes to allocate
/// @return Pointer to allocated memory, or nullptr on failure
inline void* platform_malloc(size_t size) {
    return malloc(size);
}

/// @brief Reallocates memory using the standard allocator
/// @param ptr Previously allocated memory block
/// @param size New size in bytes
/// @return Pointer to allocated memory, or nullptr on failure
inline void* platform_realloc(void* ptr, size_t size) {
    return realloc(ptr, size);
}

/// @brief Frees memory allocated by platform_malloc or platform_realloc
/// @param ptr Memory to free
inline void platform_free(void* ptr) {
    free(ptr);
}

}  // namespace micro_decoder

#endif  // ESP_PLATFORM

namespace micro_decoder {

/**
 * @brief RAII wrapper for platform-allocated memory buffers
 *
 * Owns a block of memory obtained via platform_malloc. Automatically frees on destruction.
 * Supports reallocation and move semantics. Not copyable.
 *
 * Usage:
 * 1. Construct a PlatformBuffer
 * 2. Call allocate() with the desired size
 * 3. Access memory via data()
 * 4. Buffer is automatically freed on destruction
 *
 * @code
 * PlatformBuffer buf;
 * buf.allocate(1024);
 * auto* ptr = buf.data();
 * @endcode
 */
class PlatformBuffer {
public:
    PlatformBuffer() = default;

    ~PlatformBuffer() {
        if (this->ptr_ != nullptr) {
            platform_free(this->ptr_);
        }
    }

    // Move-only
    /// @brief Move constructor; transfers ownership of the allocation from other
    PlatformBuffer(PlatformBuffer&& other) noexcept : ptr_(other.ptr_), size_(other.size_) {
        other.ptr_ = nullptr;
        other.size_ = 0;
    }

    /// @brief Move assignment operator; frees any held allocation, then takes ownership from other
    /// @return Reference to this buffer
    PlatformBuffer& operator=(PlatformBuffer&& other) noexcept {
        if (this != &other) {
            if (this->ptr_ != nullptr) {
                platform_free(this->ptr_);
            }
            this->ptr_ = other.ptr_;
            this->size_ = other.size_;
            other.ptr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    PlatformBuffer(const PlatformBuffer&) = delete;
    PlatformBuffer& operator=(const PlatformBuffer&) = delete;

    /// @brief Allocates a new buffer. Any previously held memory is freed
    /// @param size Number of bytes to allocate
    /// @return true if allocation succeeded
    bool allocate(size_t size) {
        if (this->ptr_ != nullptr) {
            platform_free(this->ptr_);
        }
        this->ptr_ = static_cast<uint8_t*>(platform_malloc(size));
        this->size_ = (this->ptr_ != nullptr) ? size : 0;
        return this->ptr_ != nullptr;
    }

    /// @brief Resizes the buffer, preserving existing data
    /// @note On failure, the original buffer remains valid
    /// @param new_size New buffer size in bytes
    /// @return true if reallocation succeeded
    bool resize(size_t new_size) {
        uint8_t* new_ptr = static_cast<uint8_t*>(platform_realloc(this->ptr_, new_size));
        if (new_ptr == nullptr) {
            return false;
        }
        this->ptr_ = new_ptr;
        this->size_ = new_size;
        return true;
    }

    /// @brief Returns true if the buffer holds an allocation
    /// @return true if the buffer holds an allocation
    explicit operator bool() const {
        return this->ptr_ != nullptr;
    }

    /// @brief Returns a pointer to the allocated buffer
    /// @return Pointer to the allocated buffer
    uint8_t* data() {
        return this->ptr_;
    }

    /// @brief Returns a const pointer to the allocated buffer
    /// @return Const pointer to the allocated buffer
    const uint8_t* data() const {
        return this->ptr_;
    }

    /// @brief Returns the current allocation size in bytes
    /// @return Current allocation size in bytes
    size_t size() const {
        return this->size_;
    }

private:
    // Pointer fields
    uint8_t* ptr_{nullptr};

    // size_t fields
    size_t size_{0};
};

}  // namespace micro_decoder
