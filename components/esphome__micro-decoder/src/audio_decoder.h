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

/// @file audio_decoder.h
/// @brief AudioDecoder: decodes FLAC/MP3/Opus/WAV audio and delivers PCM via callback

#pragma once

#include "md_transfer_buffer.h"
#include "micro_decoder/types.h"
#include "ring_buffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

// micro-flac
#ifdef MICRO_DECODER_CODEC_FLAC
#include <micro_flac/flac_decoder.h>
#endif

// micro-mp3
#ifdef MICRO_DECODER_CODEC_MP3
#include <micro_mp3/mp3_decoder.h>
#endif

// micro-opus
#ifdef MICRO_DECODER_CODEC_OPUS
#include <micro_opus/ogg_opus_decoder.h>
#endif

// micro-wav
#ifdef MICRO_DECODER_CODEC_WAV
#include <micro_wav/wav_decoder.h>
#endif

namespace micro_decoder {

/// @brief Decoder state returned after each decode iteration
enum class AudioDecoderState : uint8_t {
    DECODING = 0,  // Decode loop is running; more data expected
    FINISHED,      // All PCM has been delivered successfully
    FAILED,        // An unrecoverable decode error occurred
};

/**
 * @brief Decodes audio from a RingBuffer or in-memory buffer.
 *
 * Usage:
 *  1. Construct with desired input and output transfer buffer sizes.
 *  2. Call set_source() with either a RingBuffer* (HTTP streaming) or a
 *     const uint8_t* + length (in-memory playback).
 *  3. Call start() with the detected AudioFileType.
 *  4. Call decode() in a loop until it returns FINISHED or FAILED.
 *
 * @code
 *   AudioDecoder decoder(8192);
 *   decoder.set_source(ring_buffer);
 *   decoder.start(AudioFileType::MP3);
 *   while (decoder.decode(stop, listener, timeout_ms) == AudioDecoderState::DECODING) {}
 * @endcode
 *
 * Decoded PCM is delivered via DecoderListener::on_audio_write().
 */
class AudioDecoder {
public:
    // ========================================
    // Construction / destruction
    // ========================================

    /// @brief Constructs the decoder with the given output buffer size.
    /// @param output_buffer_size Size of the output TransferBuffer (bytes).
    explicit AudioDecoder(size_t output_buffer_size);
    /// @brief Destructor
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // ========================================
    // Source configuration: call exactly one before start()
    // ========================================

    /// @brief Source is a ring buffer (HTTP streaming path).
    /// @param ring_buffer Pointer to the ring buffer supplying encoded audio data.
    void set_source(RingBuffer* ring_buffer);

    /// @brief Source is a const in-memory buffer.
    /// @param data   Pointer to the start of the encoded audio data.
    /// @param length Total number of bytes in the buffer.
    void set_source(const uint8_t* data, size_t length);

    // ========================================
    // Lifecycle
    // ========================================

    /// @brief Initialises format-specific decoder state
    /// @param file_type The audio format to decode.
    /// @return false if the format is unsupported or memory allocation fails.
    bool start(AudioFileType file_type);

    /// @brief Decodes a chunk of audio and invokes on_audio_write via the listener
    /// @param stop_gracefully  True when the reader is done; finish remaining buffered data.
    /// @param listener         Callback target for PCM data and stream-info events.
    /// @param timeout_ms       Passed through to on_audio_write().
    /// @return DECODING while more data remains, FINISHED when all PCM has been delivered, FAILED
    /// on error.
    AudioDecoderState decode(bool stop_gracefully, DecoderListener* listener, uint32_t timeout_ms);

private:
    // ========================================
    // Internal types
    // ========================================

    /// @brief Internal per-format decode result
    enum class FileDecoderState : uint8_t {
        MORE_TO_PROCESS = 0,  // Codec produced output; call again immediately
        IDLE,                 // Codec needs more input data before it can produce output
        POTENTIALLY_FAILED,   // Codec returned an error; may recover with more data
        FAILED,               // Codec returned an unrecoverable error
        END_OF_FILE,          // Codec signalled the end of the stream
    };

    /// @brief Identifies the active audio source type
    enum class SourceMode : uint8_t {
        NONE,          // No source configured
        RING_BUFFER,   // Reading from a RingBuffer (URL playback)
        CONST_BUFFER,  // Reading from a const memory buffer
    };

    // ========================================
    // Internal helpers
    // ========================================

    // Format-specific decode steps
#ifdef MICRO_DECODER_CODEC_FLAC
    /// @brief Decodes one iteration of FLAC data
    FileDecoderState decode_flac(const uint8_t* data, size_t len, size_t& bytes_consumed);
#endif
#ifdef MICRO_DECODER_CODEC_MP3
    /// @brief Decodes one iteration of MP3 data
    FileDecoderState decode_mp3(const uint8_t* data, size_t len, size_t& bytes_consumed);
#endif
#ifdef MICRO_DECODER_CODEC_OPUS
    /// @brief Decodes one iteration of Opus data
    FileDecoderState decode_opus(const uint8_t* data, size_t len, size_t& bytes_consumed);
#endif
#ifdef MICRO_DECODER_CODEC_WAV
    /// @brief Decodes one iteration of WAV data
    FileDecoderState decode_wav(const uint8_t* data, size_t len, size_t& bytes_consumed);
#endif

    /// @brief Transfers output PCM data to the listener
    void flush_output(DecoderListener* listener, uint32_t timeout_ms);

    /// @brief Releases any outstanding ring buffer acquisition
    void release_acquired();

    // ========================================
    // Struct fields
    // ========================================
    TransferBuffer output_buffer_;
    std::optional<AudioStreamInfo> stream_info_;

    // ========================================
    // Pointer fields
    // ========================================
    const uint8_t* acquired_data_{nullptr};
    const uint8_t* const_data_{nullptr};
#ifdef MICRO_DECODER_CODEC_FLAC
    std::unique_ptr<micro_flac::FLACDecoder> flac_decoder_;
#endif
#ifdef MICRO_DECODER_CODEC_MP3
    std::unique_ptr<micro_mp3::Mp3Decoder> mp3_decoder_;
#endif
#ifdef MICRO_DECODER_CODEC_OPUS
    std::unique_ptr<micro_opus::OggOpusDecoder> opus_decoder_;
#endif
    RingBuffer* source_ring_buffer_{nullptr};
#ifdef MICRO_DECODER_CODEC_WAV
    std::unique_ptr<micro_wav::WAVDecoder> wav_decoder_;
#endif

    // ========================================
    // size_t fields
    // ========================================
    size_t acquired_available_{0};
    size_t const_data_length_{0};
    size_t const_data_pos_{0};
    size_t free_buffer_required_{0};
    size_t max_acquire_size_{0};

    // ========================================
    // 32-bit fields
    // ========================================
    uint32_t potentially_failed_count_{0};

    // ========================================
    // 8-bit fields
    // ========================================
    AudioFileType file_type_{AudioFileType::NONE};
    SourceMode source_mode_{SourceMode::NONE};

    // ========================================
    // bool fields
    // ========================================
    bool allocation_ok_{false};
    bool end_of_file_{false};
    bool stream_info_sent_{false};
};

}  // namespace micro_decoder
