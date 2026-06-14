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

/// @file decoder_source.h
/// @brief DecoderSource - top-level entry point for micro-decoder

#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace micro_decoder {

/**
 * @brief Decodes audio from an HTTP URL or in-memory buffer and delivers PCM via a callback
 *
 * Typical usage:
 * 1. Construct a DecoderSource with optional DecoderConfig.
 * 2. Call set_listener() with a DecoderListener to receive PCM data.
 * 3. Call play_url() or play_buffer() to start decoding.
 * 4. Call loop() regularly from your thread to receive on_state_change()
 *    callbacks. on_stream_info() fires on the decoder thread.
 * 5. Call stop() when finished, or let the destructor handle cleanup.
 * @code
 * MyListener listener;
 * DecoderSource decoder;
 * decoder.set_listener(&listener);
 * if (decoder.play_url("http://example.com/song.mp3")) {
 *     while (decoder.state() == DecoderState::IDLE) {
 *         decoder.loop();  // pump until no longer IDLE
 *     }
 *     while (decoder.state() == DecoderState::PLAYING) {
 *         decoder.loop();
 *     }
 *     decoder.stop();
 * }
 * @endcode
 *
 * Non-copyable, non-movable. Uses pImpl to hide platform internals.
 */
class DecoderSource {
public:
    /// @brief Constructs the decoder source with optional configuration
    /// @param config Configuration (buffer sizes, timeouts)
    explicit DecoderSource(const DecoderConfig& config = {});

    /// @brief Stops playback and releases all resources
    ~DecoderSource();

    // Not copyable or movable
    DecoderSource(const DecoderSource&) = delete;
    DecoderSource& operator=(const DecoderSource&) = delete;
    DecoderSource(DecoderSource&&) = delete;
    DecoderSource& operator=(DecoderSource&&) = delete;

    /// @brief Sets the listener that receives decoded PCM and state change events
    /// @note Must be called before play_url() or play_buffer(). Must not be called while
    /// playback is active. The listener must remain valid until stop() returns or the decoder
    /// source is destroyed.
    /// @param listener Pointer to the callback receiver, or nullptr to clear
    void set_listener(DecoderListener* listener);

    /// @brief Starts streaming and decoding audio from an HTTP/HTTPS URL
    /// Spawns a reader thread and a decoder thread.
    /// @note Calls `stop()` first if already active.
    /// @note All public methods except state() must be called from the same thread.
    /// @param url HTTP or HTTPS URL of the audio stream
    /// @return true if the threads were started successfully. Returns false
    /// (and transitions to FAILED) on initialization or allocation failure.
    /// Connection and decode errors are reported asynchronously via on_state_change(FAILED).
    bool play_url(const std::string& url);

    /// @brief Starts streaming and decoding audio from a local file
    bool play_file(const std::string& path);

    /// @brief Starts streaming a local file from the specified byte offset
    bool play_file_from_offset(const std::string& path, size_t offset);

    /// @brief Starts decoding audio from an in-memory buffer
    /// @note The buffer must remain valid until `stop()` returns or the decoder source is
    /// destroyed. Spawns a decoder thread.
    /// @note Calls `stop()` first if already active.
    /// @param data Pointer to the audio data buffer (must not be nullptr)
    /// @param length Length of the buffer in bytes (must be > 0)
    /// @param type Audio file format of the buffer contents
    /// @return false if data is null, length is zero, or the format is unsupported
    /// (AudioFileType::NONE). Returns false (and transitions to FAILED) on initialization
    /// failure. Decode errors are reported asynchronously via on_state_change(FAILED).
    bool play_buffer(const uint8_t* data, size_t length, AudioFileType type);

    /// @brief Requests a stop and waits for all threads to finish
    /// After stop() returns, the state is IDLE (if previously PLAYING or FAILED)
    /// or unchanged (if already IDLE).
    /// @note The on_state_change(IDLE) callback is deferred; it fires on the next loop() call,
    /// not before stop() returns.
    void stop();

    /// @brief Pumps pending events and fires listener callbacks on the calling thread
    /// Call regularly from your main loop. All on_state_change() callbacks fire from here.
    /// on_stream_info() and on_audio_write() fire from the decoder thread directly.
    /// Safe to call stop() or play_url()/play_buffer() from on_state_change() callbacks.
    /// @note Must be called from the same thread as play_url(), play_buffer(), and stop().
    void loop();

    /// @brief Returns the current decoder state (thread-safe)
    /// @return Current decoder state
    DecoderState state() const;

private:
    struct Impl;

    // Pointer fields
    std::unique_ptr<Impl> impl_;
};

}  // namespace micro_decoder
