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

/// @file types.h
/// @brief Public types for micro-decoder: AudioFileType, AudioStreamInfo, DecoderConfig,
/// DecoderListener, DecoderState

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef MICRO_DECODER_VERSION
#define MICRO_DECODER_VERSION "0.0.0"
#endif

namespace micro_decoder {

// ============================================================================
// Log level control (host only; no-op on ESP)
// ============================================================================

inline constexpr int LOG_LEVEL_ERROR = 1;
inline constexpr int LOG_LEVEL_WARN = 2;
inline constexpr int LOG_LEVEL_INFO = 3;
inline constexpr int LOG_LEVEL_DEBUG = 4;

/// @brief Sets the runtime log verbosity (host only; ignored on ESP)
/// @param level One of LOG_LEVEL_ERROR, LOG_LEVEL_WARN, LOG_LEVEL_INFO, or LOG_LEVEL_DEBUG
void set_log_level(int level);

// ============================================================================
// AudioFileType
// ============================================================================

/// @brief Supported audio file types
/// @note Numeric values are fixed but not part of the public API; do not serialize or persist them.
enum class AudioFileType : uint8_t {
    NONE = 0,  // Unknown, undetected, or unsupported format
#ifdef MICRO_DECODER_CODEC_FLAC
    FLAC = 1,
#endif
#ifdef MICRO_DECODER_CODEC_MP3
    MP3 = 2,
#endif
#ifdef MICRO_DECODER_CODEC_OPUS
    OPUS = 3,
#endif
#ifdef MICRO_DECODER_CODEC_WAV
    WAV = 4,
#endif
};

/// @brief Returns a human-readable string for the given AudioFileType
/// @param file_type The AudioFileType value to convert
/// @return A null-terminated string naming the codec (e.g., "WAV", "MP3", "NONE")
const char* audio_file_type_to_string(AudioFileType file_type);

/// @brief Detect audio file type from a Content-Type header value and/or URL extension
/// Tries Content-Type first, then falls back to URL extension. Either parameter may be null.
/// @note Returns AudioFileType::NONE for formats whose codec is compiled out
/// (disabled via MICRO_DECODER_CODEC_* defines).
/// @param content_type HTTP Content-Type header value, or nullptr
/// @param url URL string used as fallback via file extension, or nullptr
/// @return Detected AudioFileType, or AudioFileType::NONE if unrecognized
AudioFileType detect_audio_file_type(const char* content_type, const char* url);

// ============================================================================
// AudioStreamInfo
// ============================================================================

/**
 * @brief Describes the PCM audio format of a decoded stream
 *
 * - A sample represents one channel's unit of audio.
 * - A frame represents one sample per channel.
 */
class AudioStreamInfo {
public:
    /// @brief Constructs a default AudioStreamInfo (16-bit, mono, 16000 Hz)
    AudioStreamInfo() : AudioStreamInfo(16, 1, 16000) {}  // NOLINT(readability-magic-numbers)
    /// @brief Constructs an AudioStreamInfo with the given PCM format parameters
    /// @param bits_per_sample Bits per sample (e.g., 8, 16, 24, or 32)
    /// @param channels Number of audio channels (e.g., 1 for mono, 2 for stereo)
    /// @param sample_rate Sample rate in Hz (e.g., 44100, 48000)
    AudioStreamInfo(uint8_t bits_per_sample, uint8_t channels, uint32_t sample_rate);

    /// @brief Returns the bits per sample
    /// @return Bits per sample (8, 16, 24, or 32).
    uint8_t get_bits_per_sample() const {
        return this->bits_per_sample_;
    }

    /// @brief Returns the number of audio channels
    /// @return Number of channels (e.g., 1 for mono, 2 for stereo).
    uint8_t get_channels() const {
        return this->channels_;
    }

    /// @brief Convert frames to bytes
    /// @param frames Number of PCM frames to convert
    /// @return Number of bytes.
    size_t frames_to_bytes(uint32_t frames) const {
        return static_cast<size_t>(frames) * this->bytes_per_sample_ * this->channels_;
    }

    /// @brief Returns the sample rate
    /// @return Sample rate in Hz (e.g., 44100, 48000).
    uint32_t get_sample_rate() const {
        return this->sample_rate_;
    }

    /// @brief Convert samples to bytes
    /// @param samples Number of PCM samples to convert
    /// @return Number of bytes.
    size_t samples_to_bytes(uint32_t samples) const {
        return static_cast<size_t>(samples) * this->bytes_per_sample_;
    }

    /// @brief Returns true if both AudioStreamInfo objects describe the same PCM format
    /// @param rhs The AudioStreamInfo to compare against
    /// @return true if the PCM formats match
    bool operator==(const AudioStreamInfo& rhs) const;
    /// @brief Returns true if the two AudioStreamInfo objects describe different PCM formats
    /// @param rhs The AudioStreamInfo to compare against
    /// @return true if the PCM formats differ
    bool operator!=(const AudioStreamInfo& rhs) const {
        return !this->operator==(rhs);
    }

private:
    // 32-bit fields
    uint32_t sample_rate_{0};

    // 8-bit fields
    uint8_t bits_per_sample_{0};
    uint8_t bytes_per_sample_{0};
    uint8_t channels_{0};
};

// ============================================================================
// DecoderState
// ============================================================================

/// @brief Decoder lifecycle states
enum class DecoderState : uint8_t {
    IDLE = 0,     ///< Not playing; initial state and state after stop() completes
    PLAYING = 1,  ///< Decoder is actively producing audio
    FAILED = 2,   ///< An error occurred; call play_*() to start a new stream
};

// ============================================================================
// DecoderListener
// ============================================================================

/**
 * @brief Callback interface for DecoderSource events.
 *
 * Implement this interface and pass a pointer to DecoderSource::set_listener().
 *
 * Usage:
 *  1. Subclass DecoderListener and override the three pure-virtual methods.
 *  2. Construct your listener and call DecoderSource::set_listener() before
 *     calling play_url() or play_buffer().
 *  3. on_stream_info() fires once per stream; on_audio_write() fires
 *     repeatedly with PCM frames; on_state_change() fires on every
 *     DecoderState transition.
 *
 * ## Threading model
 *
 * - on_state_change() is called exclusively from the thread that calls
 *   DecoderSource::loop(). It is safe to call stop(), play_url(), or
 *   play_buffer() from this callback.
 * - on_stream_info() and on_audio_write() are called from the decoder
 *   thread. Do NOT call DecoderSource::stop(), play_url(), or
 *   play_buffer() from these callbacks.
 *
 * @code
 * struct MyListener : micro_decoder::DecoderListener {
 *     void on_stream_info(const micro_decoder::AudioStreamInfo& info) override {
 *         // configure your audio sink here
 *     }
 *     size_t on_audio_write(const uint8_t* data, size_t length, uint32_t timeout_ms) override {
 *         // write PCM data to your sink; return bytes actually consumed
 *         return length;
 *     }
 *     void on_state_change(micro_decoder::DecoderState state) override {
 *         // react to PLAYING, FAILED, IDLE transitions
 *     }
 * };
 * @endcode
 */
class DecoderListener {
public:
    virtual ~DecoderListener() = default;

    /// @brief Called exactly once per play_url() or play_buffer() call, before the first
    /// on_audio_write(). Called from the decoder thread. Must not call DecoderSource::stop(),
    /// play_url(), or play_buffer().
    /// @param info PCM format of the decoded stream.
    virtual void on_stream_info(const AudioStreamInfo& info) = 0;

    /// @brief Called with decoded PCM data from the decoder thread
    /// May block up to timeout_ms waiting for the sink to accept data.
    /// Returning fewer than `length` bytes applies backpressure;
    /// the decoder will retry with the unconsumed remainder.
    /// Must not call DecoderSource::stop(), play_url(), or play_buffer().
    /// @param data    Pointer to the decoded PCM bytes (read-only; valid only for the
    ///                 duration of this callback).
    /// @param length  Number of bytes available in `data`.
    /// @param timeout_ms  Maximum time in milliseconds to block waiting for the sink.
    /// @return Number of bytes actually consumed (must be <= length).
    virtual size_t on_audio_write(const uint8_t* data, size_t length, uint32_t timeout_ms) = 0;

    /// @brief Called when the decoder transitions to a new state
    /// Called exclusively from the thread invoking DecoderSource::loop().
    /// @param state The new DecoderState value.
    virtual void on_state_change(DecoderState state) = 0;
};

// ============================================================================
// DecoderConfig
// ============================================================================

/// @brief Configuration for DecoderSource
struct DecoderConfig {
    /// @brief HTTP User-Agent header value sent with streaming requests.
    /// Defaults to "micro-decoder/<version> (https://github.com/esphome-libs/micro-decoder)".
    /// Set to an empty string to fall back to the underlying HTTP client's default.
    std::string http_user_agent{"micro-decoder/" MICRO_DECODER_VERSION
                                " (https://github.com/esphome-libs/micro-decoder)"};

    /// @brief PEM-encoded CA certificate(s) used to verify HTTPS server certificates.
    /// When empty, the platform default is used: on ESP-IDF the built-in
    /// MbedTLS certificate bundle (when CONFIG_MBEDTLS_CERTIFICATE_BUNDLE is
    /// enabled); on host (libcurl) the system trust store.
    /// When non-empty, the string must contain one or more PEM-formatted
    /// certificates and is used as the sole trust anchor for HTTPS requests.
    /// Ignored for plain HTTP URLs.
    std::string http_ca_certificate;

    /// @brief Size of the ring buffer between reader and decoder threads (bytes)
    size_t ring_buffer_size{static_cast<size_t>(48) * 1024};  // NOLINT(readability-magic-numbers)

    /// @brief Size of the HTTP read buffer (AudioReader) and output staging buffer
    /// (AudioDecoder) in bytes. The decoder may reallocate its copy larger if needed.
    size_t transfer_buffer_size{8192};  // NOLINT(readability-magic-numbers)

    /// @brief HTTP connection/read timeout in milliseconds
    uint32_t http_timeout_ms{5000};  // NOLINT(readability-magic-numbers)

    /// @brief Maximum time to block in on_audio_write() per call (milliseconds)
    uint32_t audio_write_timeout_ms{25};  // NOLINT(readability-magic-numbers)

    /// @brief Maximum time the reader blocks writing to the ring buffer per call (milliseconds)
    uint32_t reader_write_timeout_ms{25};  // NOLINT(readability-magic-numbers)

    /// @brief Size of the ESP-IDF HTTP client receive buffer in bytes (ESP-IDF only)
    size_t http_rx_buffer_size{2048};

    // ========================================
    // ESP-IDF thread config (ignored on host)
    // ========================================

    /// @brief Stack size for the reader thread (bytes, ESP-IDF only)
    size_t reader_stack_size{5120};  // NOLINT(readability-magic-numbers)

    /// @brief Stack size for the decoder thread (bytes, ESP-IDF only)
    size_t decoder_stack_size{5120};  // NOLINT(readability-magic-numbers)

    /// @brief FreeRTOS priority for the reader thread (ESP-IDF only)
    int reader_priority{2};

    /// @brief FreeRTOS priority for the decoder thread (ESP-IDF only)
    int decoder_priority{2};

    /// @brief Allocate the decoder task stack in PSRAM (ESP-IDF only)
    /// The reader task stack is always allocated in internal RAM.
    bool decoder_stack_in_psram{false};
};

}  // namespace micro_decoder
