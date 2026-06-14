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

#include "micro_decoder/decoder_source.h"

#include "audio_decoder.h"
#include "audio_reader.h"
#include "platform/event_flags.h"
#include "platform/logging.h"
#include "platform/thread.h"
#include "ring_buffer.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <new>
#include <thread>

namespace micro_decoder {

static constexpr const char* TAG = "micro_decoder.decoder_source";

// ============================================================================
// Event flag bits
// ============================================================================

static constexpr uint32_t FLAG_READER_READY = (1U << 0);
static constexpr uint32_t FLAG_READER_FINISHED = (1U << 1);
static constexpr uint32_t FLAG_READER_ERROR = (1U << 2);
static constexpr uint32_t FLAG_COMMAND_STOP = (1U << 3);
static constexpr uint32_t FLAG_DECODER_STARTED = (1U << 4);
static constexpr uint32_t FLAG_DECODER_FINISHED = (1U << 5);
static constexpr uint32_t FLAG_DECODER_FAILED = (1U << 6);

// ============================================================================
// DecoderSource::Impl
// ============================================================================

/// @brief Internal implementation state for DecoderSource (pImpl)
struct DecoderSource::Impl {
    // Struct fields
    DecoderConfig config;
    std::thread decoder_thread;
    EventFlags event_flags;
    std::thread reader_thread;
    RingBuffer ring_buffer;

    // Pointer fields
    std::atomic<DecoderListener*> listener{nullptr};

    // 8-bit fields
    std::atomic<DecoderState> decoder_state{DecoderState::IDLE};
    std::atomic<AudioFileType> detected_file_type{AudioFileType::NONE};

    // bool fields
    bool initialized_{false};
    std::atomic<bool> pending_state_notification_{false};
    std::atomic<bool> stop_in_progress_{false};

    explicit Impl(const DecoderConfig& cfg)
        : config(cfg), initialized_(this->event_flags.create()) {}

    /// @brief Updates the decoder state and notifies the listener immediately
    /// Only called from pump_events(), which runs on the user's thread via loop().
    void set_state(DecoderState s) {
        this->decoder_state.store(s, std::memory_order_release);
        DecoderListener* l = this->listener.load(std::memory_order_acquire);
        if (l != nullptr) {
            l->on_state_change(s);
        }
    }

    /// @brief Updates the decoder state without notifying the listener.
    /// The notification is deferred to the next pump_events() call via loop().
    void store_state(DecoderState s) {
        this->decoder_state.store(s, std::memory_order_release);
        this->pending_state_notification_.store(true, std::memory_order_release);
    }

    /// @brief Reads pending event flags and fires listener callbacks
    /// Called from DecoderSource::loop() on the user's thread.
    void pump_events() {
        // Drain deferred state notifications from stop() or play_*() error paths
        if (this->pending_state_notification_.exchange(false, std::memory_order_acq_rel)) {
            DecoderState s = this->decoder_state.load(std::memory_order_acquire);
            DecoderListener* l = this->listener.load(std::memory_order_acquire);
            if (l != nullptr) {
                l->on_state_change(s);
            }
        }

        uint32_t flags = this->event_flags.get();

        // Check FAILED first -- if the decoder failed, skip STARTED/FINISHED to avoid
        // spurious PLAYING or IDLE callbacks before the FAILED transition.
        if (flags & FLAG_DECODER_FAILED) {
            this->event_flags.clear(FLAG_DECODER_FAILED | FLAG_DECODER_STARTED |
                                    FLAG_DECODER_FINISHED);
            this->set_state(DecoderState::FAILED);
        } else {
            if (flags & FLAG_DECODER_STARTED) {
                this->event_flags.clear(FLAG_DECODER_STARTED);
                DecoderState current = this->decoder_state.load(std::memory_order_acquire);
                if (current != DecoderState::FAILED) {
                    this->set_state(DecoderState::PLAYING);
                }
            }

            if (flags & FLAG_DECODER_FINISHED) {
                this->event_flags.clear(FLAG_DECODER_FINISHED);
                DecoderState current = this->decoder_state.load(std::memory_order_acquire);
                if (current == DecoderState::PLAYING) {
                    this->set_state(DecoderState::IDLE);
                }
            }
        }
    }

    // ========================================
    // HTTP reader thread body
    // ========================================

    /// @brief Reader thread entry point that streams from an HTTP URL
    void reader_thread_func(const std::string& url) {
        AudioReader reader(this->config.transfer_buffer_size, this->config.http_timeout_ms,
                           this->config.reader_write_timeout_ms, this->config.http_rx_buffer_size,
                           this->config.http_user_agent, this->config.http_ca_certificate);
        reader.set_sink(&this->ring_buffer);

        if (!reader.start_url(url)) {
            MD_LOGE(TAG, "Reader failed to open URL");
            this->event_flags.set(FLAG_READER_ERROR);
            return;
        }

        // Signal the file type to the decoder
        this->detected_file_type.store(reader.file_type(), std::memory_order_release);
        this->event_flags.set(FLAG_READER_READY);

        while (true) {
            // Check for stop command
            if (this->event_flags.get() & FLAG_COMMAND_STOP) {
                break;
            }

            AudioReaderState rs = reader.run();
            if (rs == AudioReaderState::FINISHED) {
                MD_LOGD(TAG, "Reader finished");
                this->event_flags.set(FLAG_READER_FINISHED);
                break;
            }
            if (rs == AudioReaderState::FAILED) {
                MD_LOGE(TAG, "Reader error");
                this->event_flags.set(FLAG_READER_ERROR);
                break;
            }

            if (rs == AudioReaderState::IDLE) {
                // No data available from HTTP; yield to avoid a tight spin.
                // Wakes immediately on a stop command.
                this->event_flags.wait(FLAG_COMMAND_STOP, false, false,
                                       this->config.reader_write_timeout_ms);
            }
        }
    }

    /// @brief Reader thread entry point that streams from a local file
    void file_reader_thread_func(const std::string& path, size_t offset) {
        AudioFileType file_type = detect_audio_file_type(nullptr, path.c_str());
        if (file_type == AudioFileType::NONE) {
            MD_LOGE(TAG, "Unsupported local audio file: %s", path.c_str());
            this->event_flags.set(FLAG_READER_ERROR);
            return;
        }

        FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            MD_LOGE(TAG, "Failed to open local audio file: %s", path.c_str());
            this->event_flags.set(FLAG_READER_ERROR);
            return;
        }
        if (offset != 0 && std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
            MD_LOGE(TAG, "Failed to seek local audio file: %s", path.c_str());
            std::fclose(file);
            this->event_flags.set(FLAG_READER_ERROR);
            return;
        }

        std::unique_ptr<uint8_t[]> buffer(new (std::nothrow)
                                             uint8_t[this->config.transfer_buffer_size]);
        if (!buffer) {
            std::fclose(file);
            this->event_flags.set(FLAG_READER_ERROR);
            return;
        }

        this->detected_file_type.store(file_type, std::memory_order_release);
        this->event_flags.set(FLAG_READER_READY);

        while (!(this->event_flags.get() & FLAG_COMMAND_STOP)) {
            size_t bytes_read =
                std::fread(buffer.get(), 1, this->config.transfer_buffer_size, file);
            size_t written = 0;
            while (written < bytes_read &&
                   !(this->event_flags.get() & FLAG_COMMAND_STOP)) {
                written += this->ring_buffer.write(buffer.get() + written, bytes_read - written,
                                                   this->config.reader_write_timeout_ms);
            }

            if (bytes_read < this->config.transfer_buffer_size) {
                if (std::ferror(file)) {
                    MD_LOGE(TAG, "Failed reading local audio file: %s", path.c_str());
                    this->event_flags.set(FLAG_READER_ERROR);
                } else {
                    this->event_flags.set(FLAG_READER_FINISHED);
                }
                break;
            }
        }

        std::fclose(file);
    }

    // ========================================
    // Decoder thread body (HTTP path)
    // ========================================

    /// @brief Decoder thread entry point for URL-based playback
    void decoder_thread_func_url() {
        // Wait for the reader to signal file type or error
        static constexpr uint32_t WAIT_MARGIN_MS = 2000;
        uint32_t bits =
            this->event_flags.wait(FLAG_READER_READY | FLAG_READER_ERROR | FLAG_COMMAND_STOP, false,
                                   false, this->config.http_timeout_ms + WAIT_MARGIN_MS);

        if (bits & FLAG_COMMAND_STOP) {
            return;
        }
        if (bits & FLAG_READER_ERROR) {
            this->event_flags.set(FLAG_DECODER_FAILED);
            return;
        }

        AudioFileType file_type = this->detected_file_type.load(std::memory_order_acquire);

        this->run_decoder(file_type);
    }

    // ========================================
    // Common decoder logic - HTTP streaming source
    // ========================================

    /// @brief Runs the decode loop for URL-sourced audio
    void run_decoder(AudioFileType file_type) {
        AudioDecoder decoder(this->config.transfer_buffer_size);
        decoder.set_source(&this->ring_buffer);

        if (!decoder.start(file_type)) {
            MD_LOGE(TAG, "Decoder start failed for %s", audio_file_type_to_string(file_type));
            this->event_flags.set(FLAG_DECODER_FAILED);
            return;
        }

        this->event_flags.set(FLAG_DECODER_STARTED);

        while (true) {
            uint32_t flags = this->event_flags.get();

            if (flags & FLAG_COMMAND_STOP) {
                break;
            }

            bool reader_done = (flags & (FLAG_READER_FINISHED | FLAG_READER_ERROR)) != 0;

            AudioDecoderState ds =
                decoder.decode(reader_done, this->listener.load(std::memory_order_acquire),
                               this->config.audio_write_timeout_ms);

            if (ds == AudioDecoderState::FINISHED) {
                if (flags & FLAG_READER_ERROR) {
                    MD_LOGE(TAG, "Decode drained after reader error");
                    this->event_flags.set(FLAG_DECODER_FAILED);
                } else {
                    MD_LOGD(TAG, "Decode finished");
                    this->event_flags.set(FLAG_DECODER_FINISHED);
                }
                return;
            }
            if (ds == AudioDecoderState::FAILED) {
                MD_LOGE(TAG, "Decode failed");
                this->event_flags.set(FLAG_DECODER_FAILED);
                return;
            }
        }
        // Stopped by command; stop() handles the IDLE transition
    }

    // ========================================
    // Common decoder logic - in-memory source
    // ========================================

    /// @brief Runs the decode loop for buffer-sourced audio
    void run_decoder_buffer(const uint8_t* data, size_t length, AudioFileType file_type) {
        AudioDecoder decoder(this->config.transfer_buffer_size);
        decoder.set_source(data, length);

        if (!decoder.start(file_type)) {
            MD_LOGE(TAG, "Decoder start failed for %s", audio_file_type_to_string(file_type));
            this->event_flags.set(FLAG_DECODER_FAILED);
            return;
        }

        this->event_flags.set(FLAG_DECODER_STARTED);

        while (true) {
            if (this->event_flags.get() & FLAG_COMMAND_STOP) {
                break;
            }

            AudioDecoderState ds =
                decoder.decode(true, this->listener.load(std::memory_order_acquire),
                               this->config.audio_write_timeout_ms);

            if (ds == AudioDecoderState::FINISHED) {
                MD_LOGD(TAG, "Decode finished");
                this->event_flags.set(FLAG_DECODER_FINISHED);
                return;
            }
            if (ds == AudioDecoderState::FAILED) {
                MD_LOGE(TAG, "Decode failed");
                this->event_flags.set(FLAG_DECODER_FAILED);
                return;
            }
        }
        // Stopped by command; stop() handles the IDLE transition
    }
};

// ============================================================================
// DecoderSource
// ============================================================================

DecoderSource::DecoderSource(const DecoderConfig& config)
    : impl_(std::make_unique<DecoderSource::Impl>(config)) {}

DecoderSource::~DecoderSource() {
    this->stop();
}

void DecoderSource::set_listener(DecoderListener* listener) {
    this->impl_->listener.store(listener, std::memory_order_release);
}

bool DecoderSource::play_url(const std::string& url) {
    if (!this->impl_->initialized_) {
        MD_LOGE(TAG, "Not initialized (event flags allocation failed)");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    this->stop();

    if (!this->impl_->ring_buffer.create(this->impl_->config.ring_buffer_size)) {
        MD_LOGE(TAG, "Failed to allocate ring buffer");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    // Clear event flags and pending notifications from any previous run
    this->impl_->event_flags.clear(FLAG_READER_READY | FLAG_READER_FINISHED | FLAG_READER_ERROR |
                                   FLAG_COMMAND_STOP | FLAG_DECODER_STARTED |
                                   FLAG_DECODER_FINISHED | FLAG_DECODER_FAILED);
    this->impl_->pending_state_notification_.store(false, std::memory_order_release);

    // Spawn reader thread. Always in internal RAM for lwip settings compatibility
    platform_configure_thread("md_reader", this->impl_->config.reader_stack_size,
                              this->impl_->config.reader_priority, false);
    // String copy occurs during lambda construction, not in noexcept thread
    this->impl_->reader_thread =
        // NOLINTNEXTLINE(bugprone-exception-escape)
        std::thread([this, url]() noexcept { this->impl_->reader_thread_func(url); });

    // Spawn decoder thread
    platform_configure_thread("md_decoder", this->impl_->config.decoder_stack_size,
                              this->impl_->config.decoder_priority,
                              this->impl_->config.decoder_stack_in_psram);
    this->impl_->decoder_thread =
        std::thread([this]() noexcept { this->impl_->decoder_thread_func_url(); });

    return true;
}

bool DecoderSource::play_file(const std::string& path) {
    return this->play_file_from_offset(path, 0);
}

bool DecoderSource::play_file_from_offset(const std::string& path, size_t offset) {
    if (!this->impl_->initialized_ || path.empty()) {
        MD_LOGE(TAG, "Invalid local file playback request");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    this->stop();

    if (!this->impl_->ring_buffer.create(this->impl_->config.ring_buffer_size)) {
        MD_LOGE(TAG, "Failed to allocate ring buffer");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    this->impl_->event_flags.clear(FLAG_READER_READY | FLAG_READER_FINISHED | FLAG_READER_ERROR |
                                   FLAG_COMMAND_STOP | FLAG_DECODER_STARTED |
                                   FLAG_DECODER_FINISHED | FLAG_DECODER_FAILED);
    this->impl_->pending_state_notification_.store(false, std::memory_order_release);

    platform_configure_thread("md_file_reader", this->impl_->config.reader_stack_size,
                              this->impl_->config.reader_priority, false);
    this->impl_->reader_thread = std::thread(
        [this, path, offset]() noexcept { this->impl_->file_reader_thread_func(path, offset); });

    platform_configure_thread("md_decoder", this->impl_->config.decoder_stack_size,
                              this->impl_->config.decoder_priority,
                              this->impl_->config.decoder_stack_in_psram);
    this->impl_->decoder_thread =
        std::thread([this]() noexcept { this->impl_->decoder_thread_func_url(); });

    return true;
}

bool DecoderSource::play_buffer(const uint8_t* data, size_t length, AudioFileType type) {
    if (data == nullptr || length == 0) {
        MD_LOGE(TAG, "Null or empty buffer");
        return false;
    }

    if (type == AudioFileType::NONE) {
        MD_LOGE(TAG, "Unsupported audio file type");
        return false;
    }

    if (!this->impl_->initialized_) {
        MD_LOGE(TAG, "Not initialized (event flags allocation failed)");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    this->stop();

    // Clear event flags and pending notifications from any previous run
    this->impl_->event_flags.clear(FLAG_READER_READY | FLAG_READER_FINISHED | FLAG_READER_ERROR |
                                   FLAG_COMMAND_STOP | FLAG_DECODER_STARTED |
                                   FLAG_DECODER_FINISHED | FLAG_DECODER_FAILED);
    this->impl_->pending_state_notification_.store(false, std::memory_order_release);

    // Spawn decoder thread only
    platform_configure_thread("md_decoder", this->impl_->config.decoder_stack_size,
                              this->impl_->config.decoder_priority,
                              this->impl_->config.decoder_stack_in_psram);
    this->impl_->decoder_thread = std::thread([this, data, length, type]() noexcept {
        this->impl_->run_decoder_buffer(data, length, type);
    });

    return true;
}

void DecoderSource::stop() {
    // Re-entrancy guard: prevent recursive stop() from on_state_change callbacks
    bool expected = false;
    if (!this->impl_->stop_in_progress_.compare_exchange_strong(expected, true)) {
        return;
    }

    DecoderState current = this->impl_->decoder_state.load(std::memory_order_acquire);

    // Signal all threads to stop
    this->impl_->event_flags.set(FLAG_COMMAND_STOP);

    if (this->impl_->reader_thread.joinable()) {
        this->impl_->reader_thread.join();
    }
    if (this->impl_->decoder_thread.joinable()) {
        this->impl_->decoder_thread.join();
    }

    // Clear all pending events; threads are done, no more will arrive
    this->impl_->event_flags.clear(FLAG_READER_READY | FLAG_READER_FINISHED | FLAG_READER_ERROR |
                                   FLAG_COMMAND_STOP | FLAG_DECODER_STARTED |
                                   FLAG_DECODER_FINISHED | FLAG_DECODER_FAILED);

    if (current == DecoderState::PLAYING || current == DecoderState::FAILED) {
        this->impl_->store_state(DecoderState::IDLE);
    }

    this->impl_->stop_in_progress_.store(false, std::memory_order_release);
}

void DecoderSource::loop() {
    this->impl_->pump_events();
}

DecoderState DecoderSource::state() const {
    return this->impl_->decoder_state.load(std::memory_order_acquire);
}

}  // namespace micro_decoder
