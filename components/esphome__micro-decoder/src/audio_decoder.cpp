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

#include "audio_decoder.h"

#include "platform/logging.h"

#include <cinttypes>

namespace micro_decoder {

static constexpr const char* TAG = "micro_decoder.audio_decoder";

static constexpr uint32_t MAX_POTENTIALLY_FAILED_COUNT = 10;
static constexpr uint32_t READ_TIMEOUT_MS = 20;

// ============================================================================
// Construction / destruction
// ============================================================================

AudioDecoder::AudioDecoder(size_t output_buffer_size)
    : max_acquire_size_(output_buffer_size),
      allocation_ok_(this->output_buffer_.allocate(output_buffer_size)) {}

AudioDecoder::~AudioDecoder() {
    this->release_acquired();
}

// ============================================================================
// Source configuration: call exactly one before start()
// ============================================================================

void AudioDecoder::set_source(RingBuffer* ring_buffer) {
    this->source_mode_ = SourceMode::RING_BUFFER;
    this->source_ring_buffer_ = ring_buffer;
}

void AudioDecoder::set_source(const uint8_t* data, size_t length) {
    this->source_mode_ = SourceMode::CONST_BUFFER;
    this->const_data_ = data;
    this->const_data_length_ = length;
    this->const_data_pos_ = 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool AudioDecoder::start(AudioFileType file_type) {
    if (!this->allocation_ok_) {
        MD_LOGE(TAG, "Output buffer allocation failed");
        return false;
    }
    this->file_type_ = file_type;
    this->stream_info_.reset();
    this->stream_info_sent_ = false;
    this->end_of_file_ = false;
    this->potentially_failed_count_ = 0;
    this->acquired_data_ = nullptr;
    this->acquired_available_ = 0;
    switch (file_type) {
#ifdef MICRO_DECODER_CODEC_FLAC
        case AudioFileType::FLAC:
            this->flac_decoder_ = std::make_unique<micro_flac::FLACDecoder>();
            this->flac_decoder_->set_crc_check_enabled(false);
            this->free_buffer_required_ = this->output_buffer_.capacity();
            break;
#endif

#ifdef MICRO_DECODER_CODEC_MP3
        case AudioFileType::MP3:
            this->mp3_decoder_ = std::make_unique<micro_mp3::Mp3Decoder>();
            this->free_buffer_required_ = micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES;
            if (this->output_buffer_.capacity() < this->free_buffer_required_) {
                if (!this->output_buffer_.allocate(this->free_buffer_required_)) {
                    return false;
                }
            }
            break;
#endif

#ifdef MICRO_DECODER_CODEC_OPUS
        case AudioFileType::OPUS:
            this->opus_decoder_ = std::make_unique<micro_opus::OggOpusDecoder>();
            this->free_buffer_required_ = this->output_buffer_.capacity();
            break;
#endif

#ifdef MICRO_DECODER_CODEC_WAV
        case AudioFileType::WAV:
            this->wav_decoder_ = std::make_unique<micro_wav::WAVDecoder>();
            this->free_buffer_required_ = 1024;
            if (this->output_buffer_.capacity() < this->free_buffer_required_) {
                if (!this->output_buffer_.allocate(this->free_buffer_required_)) {
                    return false;
                }
            }
            break;
#endif

        case AudioFileType::NONE:
        default:
            MD_LOGE(TAG, "Unsupported audio file type");
            return false;
    }

    return true;
}

AudioDecoderState AudioDecoder::decode(bool stop_gracefully, DecoderListener* listener,
                                       uint32_t timeout_ms) {
    if (this->source_mode_ == SourceMode::NONE) {
        return AudioDecoderState::FAILED;
    }

    // Graceful stop: if output and input are both empty and EOF was signalled, we're done
    if (stop_gracefully) {
        if (this->output_buffer_.available() == 0 && this->end_of_file_) {
            this->release_acquired();
            return AudioDecoderState::FINISHED;
        }
        bool input_empty = false;
        if (this->source_mode_ == SourceMode::CONST_BUFFER) {
            input_empty = (this->const_data_pos_ >= this->const_data_length_);
        } else {
            input_empty =
                (this->acquired_available_ == 0) && (this->source_ring_buffer_->available() == 0);
        }
        if (this->output_buffer_.available() == 0 && input_empty) {
            this->release_acquired();
            return AudioDecoderState::FINISHED;
        }
    }

    if (this->potentially_failed_count_ > MAX_POTENTIALLY_FAILED_COUNT) {
        this->release_acquired();
        return AudioDecoderState::FAILED;
    }

    // Flush any decoded output first
    this->flush_output(listener, timeout_ms);

    FileDecoderState state = FileDecoderState::MORE_TO_PROCESS;
    bool first_iteration = true;
    size_t bytes_consumed = 0;
    size_t prev_bytes_consumed = 0;

    while (state == FileDecoderState::MORE_TO_PROCESS) {
        // Check if we have enough output space
        if (this->output_buffer_.free() < this->free_buffer_required_) {
            this->flush_output(listener, timeout_ms);
            if (this->output_buffer_.free() < this->free_buffer_required_) {
                break;
            }
        }

        // Obtain a pointer to the next chunk of input
        const uint8_t* input_data = nullptr;
        size_t input_len = 0;

        if (this->source_mode_ == SourceMode::RING_BUFFER) {
            // Use existing acquired data, or acquire new data from the ring buffer
            if (this->acquired_available_ == 0) {
                this->release_acquired();
                this->source_ring_buffer_->receive_acquire(
                    &input_data, &input_len, this->max_acquire_size_, READ_TIMEOUT_MS);
                if (input_len > 0) {
                    this->acquired_data_ = input_data;
                    this->acquired_available_ = input_len;
                }
            }
            input_data = this->acquired_data_;
            input_len = this->acquired_available_;
        } else {
            size_t remaining = this->const_data_length_ - this->const_data_pos_;
            if (remaining > 0) {
                input_data = this->const_data_ + this->const_data_pos_;
                input_len = remaining;
            }
        }

        if (this->source_mode_ == SourceMode::RING_BUFFER && !first_iteration &&
            input_len < prev_bytes_consumed) {
            // Ring buffer yielded a smaller chunk than what was consumed last iteration;
            // break to let more data accumulate before retrying
            break;
        }

        if (input_len == 0) {
            state = FileDecoderState::IDLE;
        } else {
            bytes_consumed = 0;
            switch (this->file_type_) {
#ifdef MICRO_DECODER_CODEC_FLAC
                case AudioFileType::FLAC:
                    state = this->decode_flac(input_data, input_len, bytes_consumed);
                    break;
#endif
#ifdef MICRO_DECODER_CODEC_MP3
                case AudioFileType::MP3:
                    state = this->decode_mp3(input_data, input_len, bytes_consumed);
                    break;
#endif
#ifdef MICRO_DECODER_CODEC_OPUS
                case AudioFileType::OPUS:
                    state = this->decode_opus(input_data, input_len, bytes_consumed);
                    break;
#endif
#ifdef MICRO_DECODER_CODEC_WAV
                case AudioFileType::WAV:
                    state = this->decode_wav(input_data, input_len, bytes_consumed);
                    break;
#endif
                case AudioFileType::NONE:
                default:
                    state = FileDecoderState::IDLE;
                    break;
            }

            // Advance the input source
            if (this->source_mode_ == SourceMode::RING_BUFFER) {
                this->acquired_data_ += bytes_consumed;
                this->acquired_available_ -= bytes_consumed;
                if (this->acquired_available_ == 0) {
                    this->release_acquired();
                }
            } else {
                this->const_data_pos_ += bytes_consumed;
            }

            prev_bytes_consumed = bytes_consumed;
        }

        first_iteration = false;

        if (state == FileDecoderState::POTENTIALLY_FAILED) {
            if (bytes_consumed > 0) {
                this->potentially_failed_count_ = 0;
            } else {
                ++this->potentially_failed_count_;
            }
        } else if (state == FileDecoderState::END_OF_FILE) {
            this->end_of_file_ = true;
        } else if (state == FileDecoderState::FAILED) {
            this->release_acquired();
            return AudioDecoderState::FAILED;
        } else if (bytes_consumed > 0) {
            // Reset only on genuine forward progress (data was actually consumed)
            this->potentially_failed_count_ = 0;
        }
    }

    return AudioDecoderState::DECODING;
}

// ============================================================================
// FLAC decode
// ============================================================================

#ifdef MICRO_DECODER_CODEC_FLAC
AudioDecoder::FileDecoderState AudioDecoder::decode_flac(const uint8_t* data, size_t len,
                                                         size_t& bytes_consumed) {
    size_t codec_consumed = 0;
    size_t samples_decoded = 0;

    micro_flac::FLACDecoderResult result =
        this->flac_decoder_->decode(data, len, this->output_buffer_.get_buffer_end(),
                                    this->output_buffer_.free(), codec_consumed, samples_decoded);

    bytes_consumed = codec_consumed;

    if (result == micro_flac::FLAC_DECODER_SUCCESS) {
        if (samples_decoded > 0 && this->stream_info_.has_value()) {
            this->output_buffer_.increase_length(
                this->stream_info_.value().samples_to_bytes(samples_decoded));
        }
    } else if (result == micro_flac::FLAC_DECODER_HEADER_READY) {
        const auto& info = this->flac_decoder_->get_stream_info();
        this->stream_info_ =
            AudioStreamInfo(info.bits_per_sample(), info.num_channels(), info.sample_rate());

        this->free_buffer_required_ =
            static_cast<size_t>(this->flac_decoder_->get_output_buffer_size_samples()) *
            info.bytes_per_sample();
        if (this->output_buffer_.capacity() < this->free_buffer_required_) {
            if (!this->output_buffer_.reallocate(this->free_buffer_required_)) {
                return FileDecoderState::FAILED;
            }
        }
    } else if (result == micro_flac::FLAC_DECODER_END_OF_STREAM) {
        return FileDecoderState::END_OF_FILE;
    } else if (result == micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
        return FileDecoderState::MORE_TO_PROCESS;
    } else if (result == micro_flac::FLAC_DECODER_ERROR_OUTPUT_TOO_SMALL) {
        const auto& info = this->flac_decoder_->get_stream_info();
        this->free_buffer_required_ =
            static_cast<size_t>(this->flac_decoder_->get_output_buffer_size_samples()) *
            info.bytes_per_sample();
        if (this->output_buffer_.capacity() < this->free_buffer_required_) {
            if (!this->output_buffer_.reallocate(this->free_buffer_required_)) {
                return FileDecoderState::FAILED;
            }
        }
    } else {
        MD_LOGE(TAG, "FLAC decoder failed: %d", static_cast<int>(result));
        return FileDecoderState::POTENTIALLY_FAILED;
    }

    return FileDecoderState::MORE_TO_PROCESS;
}
#endif  // MICRO_DECODER_CODEC_FLAC

// ============================================================================
// MP3 decode
// ============================================================================

#ifdef MICRO_DECODER_CODEC_MP3
AudioDecoder::FileDecoderState AudioDecoder::decode_mp3(const uint8_t* data, size_t len,
                                                        size_t& bytes_consumed) {
    size_t codec_consumed = 0;
    size_t samples_decoded = 0;

    micro_mp3::Mp3Result result =
        this->mp3_decoder_->decode(data, len, this->output_buffer_.get_buffer_end(),
                                   this->output_buffer_.free(), codec_consumed, samples_decoded);

    bytes_consumed = codec_consumed;

    if (result == micro_mp3::MP3_OK) {
        if (samples_decoded > 0 && this->stream_info_.has_value()) {
            this->output_buffer_.increase_length(
                this->stream_info_.value().frames_to_bytes(static_cast<uint32_t>(samples_decoded)));
        }
    } else if (result == micro_mp3::MP3_STREAM_INFO_READY) {
        this->stream_info_ =
            AudioStreamInfo(this->mp3_decoder_->get_bit_depth(), this->mp3_decoder_->get_channels(),
                            this->mp3_decoder_->get_sample_rate());
    } else if (result == micro_mp3::MP3_NEED_MORE_DATA) {
        return FileDecoderState::MORE_TO_PROCESS;
    } else if (result == micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL) {
        this->free_buffer_required_ = micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES;
        if (this->output_buffer_.capacity() < this->free_buffer_required_) {
            if (!this->output_buffer_.reallocate(this->free_buffer_required_)) {
                return FileDecoderState::FAILED;
            }
        }
    } else if (result == micro_mp3::MP3_ALLOCATION_FAILED) {
        MD_LOGE(TAG, "MP3 decoder: allocation failed");
        return FileDecoderState::FAILED;
    } else {
        // MP3_DECODE_ERROR or MP3_INPUT_INVALID: recoverable
        return FileDecoderState::POTENTIALLY_FAILED;
    }

    return FileDecoderState::MORE_TO_PROCESS;
}
#endif  // MICRO_DECODER_CODEC_MP3

// ============================================================================
// Opus decode
// ============================================================================

#ifdef MICRO_DECODER_CODEC_OPUS
AudioDecoder::FileDecoderState AudioDecoder::decode_opus(const uint8_t* data, size_t len,
                                                         size_t& bytes_consumed) {
    bool was_initialized = this->opus_decoder_->is_initialized();

    size_t codec_consumed = 0;
    size_t samples_decoded = 0;

    micro_opus::OggOpusResult result =
        this->opus_decoder_->decode(data, len, this->output_buffer_.get_buffer_end(),
                                    this->output_buffer_.free(), codec_consumed, samples_decoded);

    bytes_consumed = codec_consumed;

    if (result == micro_opus::OGG_OPUS_OK) {
        if (!was_initialized && this->opus_decoder_->is_initialized()) {
            this->stream_info_ = AudioStreamInfo(this->opus_decoder_->get_bit_depth(),
                                                 this->opus_decoder_->get_channels(),
                                                 this->opus_decoder_->get_sample_rate());
        }
        if (samples_decoded > 0 && this->stream_info_.has_value()) {
            this->output_buffer_.increase_length(
                this->stream_info_.value().frames_to_bytes(static_cast<uint32_t>(samples_decoded)));
        }
    } else if (result == micro_opus::OGG_OPUS_OUTPUT_BUFFER_TOO_SMALL) {
        this->free_buffer_required_ = this->opus_decoder_->get_required_output_buffer_size();
        if (this->output_buffer_.capacity() < this->free_buffer_required_) {
            if (!this->output_buffer_.reallocate(this->free_buffer_required_)) {
                return FileDecoderState::FAILED;
            }
        }
    } else {
        MD_LOGE(TAG, "Opus decoder error: %" PRId8, result);
        return FileDecoderState::POTENTIALLY_FAILED;
    }

    return FileDecoderState::MORE_TO_PROCESS;
}
#endif  // MICRO_DECODER_CODEC_OPUS

// ============================================================================
// WAV decode
// ============================================================================

#ifdef MICRO_DECODER_CODEC_WAV
AudioDecoder::FileDecoderState AudioDecoder::decode_wav(const uint8_t* data, size_t len,
                                                        size_t& bytes_consumed) {
    size_t codec_consumed = 0;
    size_t samples_decoded = 0;

    micro_wav::WAVDecoderResult result =
        this->wav_decoder_->decode(data, len, this->output_buffer_.get_buffer_end(),
                                   this->output_buffer_.free(), codec_consumed, samples_decoded);

    bytes_consumed = codec_consumed;

    if (result == micro_wav::WAV_DECODER_SUCCESS) {
        if (samples_decoded > 0 && this->stream_info_.has_value()) {
            this->output_buffer_.increase_length(
                this->stream_info_.value().samples_to_bytes(samples_decoded));
        }
    } else if (result == micro_wav::WAV_DECODER_HEADER_READY) {
        this->stream_info_ =
            AudioStreamInfo(static_cast<uint8_t>(this->wav_decoder_->get_bits_per_sample()),
                            static_cast<uint8_t>(this->wav_decoder_->get_channels()),
                            this->wav_decoder_->get_sample_rate());
    } else if (result == micro_wav::WAV_DECODER_END_OF_STREAM) {
        return FileDecoderState::END_OF_FILE;
    } else if (result == micro_wav::WAV_DECODER_NEED_MORE_DATA) {
        // WAV just copies raw PCM. It can't partially decode a frame, so return IDLE.
        return FileDecoderState::IDLE;
    } else {
        MD_LOGE(TAG, "WAV decoder failed: %d", static_cast<int>(result));
        return FileDecoderState::POTENTIALLY_FAILED;
    }

    return FileDecoderState::MORE_TO_PROCESS;
}
#endif  // MICRO_DECODER_CODEC_WAV

// ============================================================================
// Output flushing
// ============================================================================

void AudioDecoder::flush_output(DecoderListener* listener, uint32_t timeout_ms) {
    if (listener == nullptr || this->output_buffer_.available() == 0) {
        return;
    }

    if (!this->stream_info_.has_value()) {
        return;
    }

    // Notify listener of stream info before sending any audio data
    if (!this->stream_info_sent_) {
        listener->on_stream_info(this->stream_info_.value());
        this->stream_info_sent_ = true;
    }

    const uint8_t* data = this->output_buffer_.get_buffer_start();
    size_t length = this->output_buffer_.available();

    size_t consumed = listener->on_audio_write(data, length, timeout_ms);
    if (consumed > 0) {
        this->output_buffer_.decrease_length(consumed);
    }
}

// ============================================================================
// Ring buffer acquisition helper
// ============================================================================

void AudioDecoder::release_acquired() {
    if (this->source_mode_ == SourceMode::RING_BUFFER && this->acquired_data_ != nullptr) {
        this->source_ring_buffer_->receive_release();
        this->acquired_data_ = nullptr;
        this->acquired_available_ = 0;
    }
}

}  // namespace micro_decoder
