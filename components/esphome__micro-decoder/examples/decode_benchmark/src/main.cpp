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

/* ESP32 Decode Benchmark
 *
 * Decodes ~10-second audio clips in all four supported formats (FLAC, MP3, Opus, WAV)
 * using DecoderSource::play_buffer() and reports timing statistics.
 *
 * Audio is discarded after decoding (no audio output).
 *
 * Audio source: Public domain music (Beethoven's Eroica from Musopen), 48 kHz mono.
 */

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "micro_decoder/decoder_source.h"
#include "micro_decoder/types.h"
#include "test_audio_flac.h"
#include "test_audio_mp3.h"
#include "test_audio_opus.h"
#include "test_audio_wav.h"

#include <cinttypes>
#include <cstdio>

using namespace micro_decoder;

static constexpr const char* TAG = "DECODE_BENCH";

// Audio clip descriptor
struct AudioClip {
    const char* name;
    const uint8_t* data;
    size_t len;
    AudioFileType type;
};

static constexpr AudioClip AUDIO_CLIPS[] = {
    {"FLAC", test_audio_flac_data, test_audio_flac_data_len, AudioFileType::FLAC},
    {"MP3", test_audio_mp3_data, test_audio_mp3_data_len, AudioFileType::MP3},
    {"OPUS", test_audio_opus_data, test_audio_opus_data_len, AudioFileType::OPUS},
    {"WAV", test_audio_wav_data, test_audio_wav_data_len, AudioFileType::WAV},
};
static constexpr int NUM_CLIPS = static_cast<int>(sizeof(AUDIO_CLIPS) / sizeof(AUDIO_CLIPS[0]));

// DecoderListener that discards all decoded audio
class NullSink : public DecoderListener {
public:
    void on_stream_info(const AudioStreamInfo& info) override {
        this->stream_info_ = info;
    }

    size_t on_audio_write(const uint8_t* /*data*/, size_t length,
                          uint32_t /*timeout_ms*/) override {
        this->total_bytes_ += length;
        return length;
    }

    void on_state_change(DecoderState state) override {
        if (state == DecoderState::PLAYING) {
            this->saw_playing_ = true;
        }
        if (state == DecoderState::FAILED || (state == DecoderState::IDLE && this->saw_playing_)) {
            this->done_ = true;
        }
    }

    void reset() {
        this->total_bytes_ = 0;
        this->done_ = false;
        this->saw_playing_ = false;
        this->stream_info_ = AudioStreamInfo();
    }

    AudioStreamInfo stream_info_{};
    size_t total_bytes_{0};
    bool done_{false};
    bool saw_playing_{false};
};

static void decode_clip(const AudioClip& clip, NullSink& sink) {
    sink.reset();

    DecoderConfig config;
    DecoderSource decoder(config);
    decoder.set_listener(&sink);

    int64_t start_us = esp_timer_get_time();

    bool started = decoder.play_buffer(clip.data, clip.len, clip.type);
    if (!started) {
        ESP_LOGE(TAG, "  Failed to start %s playback", clip.name);
        return;
    }

    // Poll until decoding completes via on_state_change callback.
    // Both STARTED and FINISHED flags can be processed in a single loop() call,
    // so we track completion via the listener rather than polling state().
    while (!sink.done_) {
        decoder.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    decoder.stop();

    int64_t elapsed_us = esp_timer_get_time() - start_us;
    double elapsed_ms = elapsed_us / 1000.0;

    // Compute audio duration from decoded PCM bytes
    uint32_t sample_rate = sink.stream_info_.get_sample_rate();
    uint8_t channels = sink.stream_info_.get_channels();
    uint8_t bits_per_sample = sink.stream_info_.get_bits_per_sample();
    size_t bytes_per_sample = bits_per_sample / 8;

    double audio_duration_s = 0.0;
    if (sample_rate > 0 && channels > 0 && bytes_per_sample > 0) {
        size_t bytes_per_frame = bytes_per_sample * channels;
        size_t total_frames = sink.total_bytes_ / bytes_per_frame;
        audio_duration_s = static_cast<double>(total_frames) / sample_rate;
    }

    double rtf = (audio_duration_s > 0.0) ? (elapsed_ms / 1000.0) / audio_duration_s : 0.0;

    printf("  %-5s  %7zu bytes  %8.1f ms  %5.2f s audio  RTF %.4f\n", clip.name, clip.len,
           elapsed_ms, audio_duration_s, rtf);
}

extern "C" void app_main(void) {
    printf("\n=== microDecoder Decode Benchmark ===\n");
    printf("Decoding ~10s clips, 48 kHz mono, no audio output\n\n");

    NullSink sink;
    int iteration = 0;

    while (true) {
        printf("--- Iteration %d ---\n", ++iteration);
        printf("  Codec   File size     Decode time  Duration   RTF\n");

        for (int i = 0; i < NUM_CLIPS; ++i) {
            decode_clip(AUDIO_CLIPS[i], sink);
        }

        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
