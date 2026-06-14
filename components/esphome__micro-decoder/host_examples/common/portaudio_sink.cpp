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

#include "portaudio_sink.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace micro_decoder {

// 16 KB ~= 85ms at 48kHz/16-bit/stereo
static constexpr size_t RING_BUFFER_CAPACITY = 16384;

// ============================================================================
// PortAudioRingBuffer
// ============================================================================

PortAudioRingBuffer::PortAudioRingBuffer(size_t capacity)
    : buffer_(capacity), capacity_(capacity) {}

size_t PortAudioRingBuffer::write(const uint8_t* data, size_t len) {
    size_t write_pos = this->write_pos_.load(std::memory_order_relaxed);
    size_t read_pos = this->read_pos_.load(std::memory_order_acquire);

    size_t used = (write_pos - read_pos + this->capacity_) % this->capacity_;
    size_t free_space = this->capacity_ - 1 - used;
    size_t to_write = std::min(len, free_space);

    if (to_write == 0) {
        return 0;
    }

    size_t first_chunk = std::min(to_write, this->capacity_ - (write_pos % this->capacity_));
    std::memcpy(&this->buffer_[write_pos % this->capacity_], data, first_chunk);
    if (to_write > first_chunk) {
        std::memcpy(&this->buffer_[0], data + first_chunk, to_write - first_chunk);
    }

    this->write_pos_.store((write_pos + to_write) % this->capacity_, std::memory_order_release);
    return to_write;
}

size_t PortAudioRingBuffer::read(uint8_t* dest, size_t len) {
    if (this->clear_requested_.load(std::memory_order_acquire)) {
        this->clear_requested_.store(false, std::memory_order_relaxed);
        this->read_pos_.store(this->write_pos_.load(std::memory_order_acquire),
                              std::memory_order_release);
        std::memset(dest, 0, len);
        return 0;
    }

    size_t read_pos = this->read_pos_.load(std::memory_order_relaxed);
    size_t write_pos = this->write_pos_.load(std::memory_order_acquire);

    size_t avail = (write_pos - read_pos + this->capacity_) % this->capacity_;
    size_t to_read = std::min(len, avail);

    if (to_read > 0) {
        size_t first_chunk = std::min(to_read, this->capacity_ - (read_pos % this->capacity_));
        std::memcpy(dest, &this->buffer_[read_pos % this->capacity_], first_chunk);
        if (to_read > first_chunk) {
            std::memcpy(dest + first_chunk, &this->buffer_[0], to_read - first_chunk);
        }
        this->read_pos_.store((read_pos + to_read) % this->capacity_, std::memory_order_release);
    }

    if (to_read < len) {
        std::memset(dest + to_read, 0, len - to_read);
    }

    return to_read;
}

size_t PortAudioRingBuffer::available() const {
    size_t write_pos = this->write_pos_.load(std::memory_order_acquire);
    size_t read_pos = this->read_pos_.load(std::memory_order_acquire);
    return (write_pos - read_pos + this->capacity_) % this->capacity_;
}

size_t PortAudioRingBuffer::free_space() const {
    return this->capacity_ - 1 - this->available();
}

void PortAudioRingBuffer::clear() {
    this->clear_requested_.store(true, std::memory_order_release);
}

void PortAudioRingBuffer::reset() {
    this->clear_requested_.store(false, std::memory_order_relaxed);
    this->read_pos_.store(0, std::memory_order_relaxed);
    this->write_pos_.store(0, std::memory_order_release);
}

// ============================================================================
// PortAudioSink
// ============================================================================

PortAudioSink::PortAudioSink() : ring_buffer_(RING_BUFFER_CAPACITY) {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
    }
}

PortAudioSink::~PortAudioSink() {
    stop();
    Pa_Terminate();
}

void PortAudioSink::on_stream_info(const AudioStreamInfo& info) {
    fprintf(stderr, "Stream info: %uHz %uch %ubit\n", info.get_sample_rate(), info.get_channels(),
            info.get_bits_per_sample());
    this->open_stream(info.get_sample_rate(), info.get_channels(), info.get_bits_per_sample());
}

size_t PortAudioSink::on_audio_write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    size_t total_written = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (total_written < length) {
        if (this->abort_write_.load(std::memory_order_acquire)) {
            break;
        }

        size_t written = this->ring_buffer_.write(data + total_written, length - total_written);
        total_written += written;

        if (total_written < length) {
            std::unique_lock<std::mutex> lock(this->write_mutex_);
            if (!this->write_cv_.wait_until(lock, deadline, [this]() {
                    return this->ring_buffer_.free_space() > 0 ||
                           this->abort_write_.load(std::memory_order_acquire);
                })) {
                break;
            }
        }
    }

    return total_written;
}

// No-op default; subclasses can override to react to state transitions.
void PortAudioSink::on_state_change(DecoderState /*state*/) {}

int PortAudioSink::pa_callback(const void* /*input*/, void* output, unsigned long frame_count,
                               const PaStreamCallbackTimeInfo* /*time_info*/,
                               PaStreamCallbackFlags /*status_flags*/, void* user_data) {
    auto* self = static_cast<PortAudioSink*>(user_data);
    size_t bytes_requested = frame_count * self->bytes_per_frame_;
    auto* out = static_cast<uint8_t*>(output);

    self->ring_buffer_.read(out, bytes_requested);

    self->write_cv_.notify_one();
    return paContinue;
}

bool PortAudioSink::open_stream(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    this->close_stream();
    this->abort_write_.store(false, std::memory_order_release);

    this->channels_ = channels;
    this->bits_per_sample_ = bits_per_sample;
    this->bytes_per_frame_ = static_cast<size_t>(channels) * (bits_per_sample / 8);

    PaSampleFormat sample_format = 0;
    switch (bits_per_sample) {
        case 16:
            sample_format = paInt16;
            break;
        case 24:
            sample_format = paInt24;
            break;
        case 32:
            sample_format = paInt32;
            break;
        default:
            fprintf(stderr, "PortAudio: unsupported bit depth %u\n", bits_per_sample);
            return false;
    }

    PaError err =
        Pa_OpenDefaultStream(&this->stream_, 0, this->channels_, sample_format, sample_rate,
                             paFramesPerBufferUnspecified, pa_callback, this);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio: failed to open stream: %s\n", Pa_GetErrorText(err));
        this->stream_ = nullptr;
        return false;
    }

    err = Pa_StartStream(this->stream_);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio: failed to start stream: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(this->stream_);
        this->stream_ = nullptr;
        return false;
    }

    return true;
}

void PortAudioSink::close_stream() {
    this->abort_write_.store(true, std::memory_order_release);
    this->write_cv_.notify_all();

    if (this->stream_ != nullptr) {
        Pa_StopStream(this->stream_);
        Pa_CloseStream(this->stream_);
        this->stream_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(this->write_mutex_);
    this->ring_buffer_.reset();
}

void PortAudioSink::stop() {
    this->close_stream();
}

}  // namespace micro_decoder
