# microDecoder - Audio Decoding Library

[![CI](https://github.com/esphome-libs/micro-decoder/actions/workflows/ci.yml/badge.svg)](https://github.com/esphome-libs/micro-decoder/actions/workflows/ci.yml)
[![Component Registry](https://components.espressif.com/components/esphome/micro-decoder/badge.svg)](https://components.espressif.com/components/esphome/micro-decoder)

C++ audio decoding library for ESP32 and host platforms. Reads audio from HTTP/HTTPS URLs or in-memory buffers, decodes FLAC/MP3/Opus/WAV, and delivers PCM via a callback.

[![A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/ohf-project.png)](https://www.openhomefoundation.org/)

## Features

- **HTTP/HTTPS streaming:** automatic format detection from Content-Type headers and URL extensions
- **FLAC, MP3, Opus, and WAV:** decoding via the microFLAC, microMP3, microOpus, and microWAV libraries (individually toggleable)
- **Cross-platform:** ESP-IDF (ESP32) and host (macOS/Linux)
- **Callback-driven PCM delivery:** backpressure support via blocking writes with configurable timeouts

## Usage Example

### Host (macOS/Linux)

```bash
cmake -B build
cmake --build build
```

Requires `CMake >= 3.16`, a C++17 compiler, `libcurl`, and `portaudio` (for the example player).

Codec dependencies (`micro-flac`, `micro-mp3`, `micro-opus`, `micro-wav`) are fetched automatically via CMake FetchContent.

#### Basic Player Example

The `host_examples/` directory contains a CLI player using PortAudio for audio output (`basic_player/main.cpp` with shared helpers in `common/`):

```bash
./build/host_examples/basic_player/basic_player path/to/song.mp3
./build/host_examples/basic_player/basic_player http://example.com/song.flac
```

### ESP-IDF

Used as an IDF component. Place under your project's `components/` directory or declare as a managed component in `idf_component.yml`. Add `micro_decoder` (or `micro-decoder` for managed components) to the `REQUIRES` list in your component's `CMakeLists.txt`.

See `examples/decode_benchmark/` for an [ESP32 benchmark](examples/decode_benchmark/) that decodes all four formats from flash and reports timing.

### Code Example

```cpp
#include "micro_decoder/decoder_source.h"
#include "micro_decoder/types.h"

#include <chrono>
#include <thread>

using namespace micro_decoder;

struct MyAudioSink : DecoderListener {
    void on_stream_info(const AudioStreamInfo& info) override {
        // Initialize audio hardware with stream format
    }
    size_t on_audio_write(const uint8_t* data, size_t length, uint32_t timeout_ms) override {
        // Forward decoded PCM to audio output
        return length;
    }
    void on_state_change(DecoderState state) override {
        // Handle decoder state transitions
    }
};

int main() {
    MyAudioSink sink;
    DecoderSource decoder;
    decoder.set_listener(&sink);
    if (!decoder.play_url("http://example.com/song.mp3")) {
        return 1;
    }

    while (decoder.state() == DecoderState::IDLE) {
        decoder.loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (decoder.state() == DecoderState::PLAYING) {
        decoder.loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    decoder.stop();
}
```

See the [Integration Guide](docs/INTEGRATION.md) for configuration, threading, and buffer playback.

## License

Apache 2.0

## Links

- [Integration Guide](docs/INTEGRATION.md)
- [microFLAC](https://github.com/esphome-libs/micro-flac)
- [microMP3](https://github.com/esphome-libs/micro-mp3)
- [microOpus](https://github.com/esphome-libs/micro-opus)
- [microWAV](https://github.com/esphome-libs/micro-wav)
