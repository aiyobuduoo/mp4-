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

#include "md_transfer_buffer.h"

#include <algorithm>

namespace micro_decoder {

// ============================================================================
// TransferBuffer
// ============================================================================

bool TransferBuffer::allocate(size_t size) {
    if (!this->buffer_.allocate(size)) {
        return false;
    }
    this->data_start_ = this->buffer_.data();
    this->buffer_length_ = 0;
    return true;
}

bool TransferBuffer::reallocate(size_t size) {
    size_t used_offset = static_cast<size_t>(this->data_start_ - this->buffer_.data());
    if (!this->buffer_.resize(size)) {
        return false;
    }
    this->data_start_ = this->buffer_.data() + used_offset;
    return true;
}

void TransferBuffer::decrease_length(size_t bytes) {
    bytes = std::min(bytes, this->buffer_length_);
    this->data_start_ += bytes;
    this->buffer_length_ -= bytes;

    if (this->buffer_length_ == 0) {
        this->data_start_ = this->buffer_.data();
    }
}

void TransferBuffer::increase_length(size_t bytes) {
    this->buffer_length_ += bytes;
}

}  // namespace micro_decoder
