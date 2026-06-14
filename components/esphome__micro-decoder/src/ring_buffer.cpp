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

#include "ring_buffer.h"

namespace micro_decoder {

// ============================================================================
// RingBuffer
// ============================================================================

bool RingBuffer::create(size_t size) {
    if (!this->storage_.allocate(size)) {
        return false;
    }
    return this->spsc_.create(size, this->storage_.data());
}

size_t RingBuffer::write(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    return this->spsc_.write(data, len, timeout_ms);
}

void RingBuffer::receive_acquire(const uint8_t** data, size_t* acquired_len, size_t max_len,
                                 uint32_t timeout_ms) {
    this->spsc_.receive_acquire(data, acquired_len, max_len, timeout_ms);
}

void RingBuffer::receive_release() {
    this->spsc_.receive_release();
}

}  // namespace micro_decoder
