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

/// @file portaudio_sink.h
/// @brief PortAudio-backed DecoderListener that plays decoded PCM audio

#pragma once

#include "micro_decoder/types.h"
#include <portaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace micro_decoder {

/**
 * @brief Lock-free SPSC ring buffer bridging on_audio_write() (push) and PortAudio callback (pull)
 *
 * Provides a single-producer, single-consumer ring buffer for streaming PCM audio between
 * the decoder thread and the PortAudio output callback without locking on the hot path.
 *
 * Features:
 * - Atomic read/write positions for wait-free operation in the common case
 * - Soft clear via flag (consumer-side drain on next read)
 * - Hard reset for stream teardown when no concurrent access is possible
 *
 * Basic usage:
 * @code
 * PortAudioRingBuffer rb(16384);
 * rb.write(pcm_data, num_bytes);   // producer side
 * rb.read(out_buf, frame_bytes);   // consumer side (PortAudio callback)
 * @endcode
 */
class PortAudioRingBuffer {
public:
    explicit PortAudioRingBuffer(size_t capacity);

    /// @brief Write data into the ring buffer (producer side).
    /// @param data Pointer to the source PCM bytes to write.
    /// @param len  Number of bytes to write.
    /// @return Number of bytes actually written.
    size_t write(const uint8_t* data, size_t len);

    /// @brief Read data from the ring buffer (consumer side).
    /// Fills remainder with silence if insufficient data is available.
    /// @param dest Pointer to the destination buffer to fill.
    /// @param len  Number of bytes to read.
    /// @return Number of bytes read from the ring buffer (remainder is silence-padded).
    size_t read(uint8_t* dest, size_t len);

    /// @brief Return the number of bytes currently available to read.
    /// @return Bytes available.
    size_t available() const;

    /// @brief Return the number of bytes that can be written without blocking.
    /// @return Free bytes.
    size_t free_space() const;

    /// @brief Request a drain on next read (consumer-side).
    void clear();

    /// @brief Hard reset; only safe when no concurrent readers or writers.
    void reset();

private:
    // Struct fields
    std::vector<uint8_t> buffer_;

    // size_t fields
    size_t capacity_;

    // Atomic fields
    std::atomic<size_t> read_pos_{0};
    std::atomic<size_t> write_pos_{0};
    std::atomic<bool> clear_requested_{false};
};

/**
 * @brief DecoderListener implementation that plays PCM audio via PortAudio
 *
 * Bridges the push model (on_audio_write callback) with PortAudio's pull callback
 * using a lock-free SPSC ring buffer. Blocking writes time out gracefully so the
 * decoder thread cannot stall indefinitely if the audio stream is stopped.
 *
 * Basic usage:
 * 1. Construct a PortAudioSink instance.
 * 2. Pass it as the DecoderListener when constructing a DecoderSource.
 * 3. Call DecoderSource::play_url() or play_buffer() to start playback.
 * 4. Call stop() or let the destructor handle teardown.
 *
 * @code
 * PortAudioSink sink;
 * DecoderSource decoder;
 * decoder.set_listener(&sink);
 * decoder.play_url("http://example.com/song.flac");
 * while (decoder.state() == DecoderState::IDLE) {
 *     decoder.loop();  // pump until playback starts
 * }
 * while (decoder.state() == DecoderState::PLAYING) {
 *     decoder.loop();  // pump until playback ends
 * }
 * decoder.stop();
 * sink.stop();
 * @endcode
 */
class PortAudioSink : public DecoderListener {
public:
    PortAudioSink();
    ~PortAudioSink() override;

    PortAudioSink(const PortAudioSink&) = delete;
    PortAudioSink& operator=(const PortAudioSink&) = delete;

    // DecoderListener interface
    /// @brief Open or reopen the PortAudio stream for the given format.
    /// @param info Stream parameters (sample rate, channels, bit depth).
    void on_stream_info(const AudioStreamInfo& info) override;

    /// @brief Write PCM audio into the ring buffer, blocking until space is available or timeout.
    /// @param data       Pointer to the PCM bytes to write.
    /// @param length     Number of bytes to write.
    /// @param timeout_ms Maximum time to wait for buffer space, in milliseconds.
    /// @return Number of bytes actually written.
    size_t on_audio_write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;

    /// @brief React to decoder state transitions.
    /// @param state New decoder state.
    void on_state_change(DecoderState state) override;

    /// @brief Stop playback and clear the ring buffer.
    void stop();

private:
    /// @brief PortAudio stream callback; pulls PCM from the ring buffer.
    static int pa_callback(const void* input, void* output, unsigned long frame_count,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags, void* user_data);

    /// @brief Open (or reopen) a PortAudio output stream with the given format.
    bool open_stream(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

    /// @brief Stop and close the active PortAudio stream, if any.
    void close_stream();

    // Struct fields
    PortAudioRingBuffer ring_buffer_;
    std::condition_variable write_cv_;
    std::mutex write_mutex_;

    // Pointer fields
    PaStream* stream_{nullptr};

    // size_t fields
    size_t bytes_per_frame_{0};

    // 8-bit fields
    uint8_t bits_per_sample_{0};
    uint8_t channels_{0};

    // bool fields
    std::atomic<bool> abort_write_{false};
};

}  // namespace micro_decoder
