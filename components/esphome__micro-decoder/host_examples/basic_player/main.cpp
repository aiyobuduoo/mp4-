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

/// @file basic_player/main.cpp
///
/// CLI example that plays an audio file via PortAudio using micro-decoder.
///
/// Usage:
///   basic_player <path/to/file.mp3>   - load file into memory and decode
///   basic_player <http://...>          - stream from URL
///
/// Options:
///   -v    Verbose logging
///   -q    Quiet logging (errors only)
///   -h    Show usage

#include "micro_decoder/decoder_source.h"
#include "micro_decoder/types.h"
#include "portaudio_sink.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace micro_decoder;

static constexpr int POLL_INTERVAL_MS = 10;

static std::atomic<bool> running{true};

/// @brief PortAudioSink subclass that tracks decoder state via on_state_change()
class PlayerSink : public PortAudioSink {
public:
    void on_state_change(DecoderState state) override {
        if (state == DecoderState::IDLE || state == DecoderState::FAILED) {
            this->finished_ = true;
        }
    }

    bool finished() const {
        return this->finished_;
    }

private:
    bool finished_{false};
};

static void signal_handler(int /*sig*/) {
    running.store(false);
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] <file-or-url>\n\n", prog);
    fprintf(stderr, "  file-or-url   Local audio file path or HTTP(S) URL\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -v    Verbose logging\n");
    fprintf(stderr, "  -q    Quiet logging (errors only)\n");
    fprintf(stderr, "  -h    Show this help\n");
}

// Read an entire file into memory
static std::vector<uint8_t> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        fprintf(stderr, "Cannot open file: %s\n", path);
        return {};
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return {};
    }

    std::vector<uint8_t> buf(static_cast<size_t>(size));
    size_t read = fread(buf.data(), 1, buf.size(), f);
    fclose(f);

    if (read != buf.size()) {
        fprintf(stderr, "Short read: expected %zu, got %zu\n", buf.size(), read);
        return {};
    }

    return buf;
}

int main(int argc, char* argv[]) try {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse options
    int opt_idx = 1;
    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        const char* opt = argv[opt_idx];
        if (strcmp(opt, "-v") == 0) {
            set_log_level(LOG_LEVEL_DEBUG);
        } else if (strcmp(opt, "-q") == 0) {
            set_log_level(LOG_LEVEL_ERROR);
        } else if (strcmp(opt, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", opt);
            print_usage(argv[0]);
            return 1;
        }
        ++opt_idx;
    }

    if (opt_idx >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    const char* target = argv[opt_idx];

    // Set up decoder
    DecoderConfig config;
    PlayerSink sink;
    DecoderSource decoder(config);
    decoder.set_listener(&sink);

    bool started = false;

    if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0) {
        fprintf(stderr, "Streaming URL: %s\n", target);
        started = decoder.play_url(target);
    } else {
        fprintf(stderr, "Loading file: %s\n", target);
        std::vector<uint8_t> file_data = read_file(target);
        if (file_data.empty()) {
            return 1;
        }

        AudioFileType type = detect_audio_file_type(nullptr, target);
        if (type == AudioFileType::NONE) {
            fprintf(stderr, "Unknown audio format: %s\n", target);
            return 1;
        }

        started = decoder.play_buffer(file_data.data(), file_data.size(), type);
    }

    if (!started) {
        fprintf(stderr, "Failed to start playback\n");
        return 1;
    }

    fprintf(stderr, "Press Ctrl+C to stop.\n");
    while (running.load() && !sink.finished()) {
        decoder.loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }

    fprintf(stderr, "\nStopping...\n");
    decoder.stop();
    sink.stop();

    return 0;
} catch (const std::exception& e) {
    fprintf(stderr, "Fatal error: %s\n", e.what());
    return 1;
}
