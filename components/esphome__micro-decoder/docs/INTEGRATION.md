# Integration Guide

This guide describes what you need to implement in order to integrate microDecoder into your application. The library handles HTTP streaming, audio format detection, and decoding (FLAC/MP3/Opus/WAV). You provide the PCM audio output via a listener callback.

## Overview

Integration follows this pattern:

1. Add microDecoder to your CMake build
2. Implement the `DecoderListener` interface to receive decoded PCM audio
3. Construct an `DecoderSource` with an optional `DecoderConfig`
4. Call `play_url()` or `play_buffer()` to start decoding
5. Call `loop()` regularly from your thread to receive state callbacks
6. Call `stop()` when done

All three listener methods -- `on_stream_info()`, `on_audio_write()`, and `on_state_change()` -- are pure virtual and must be implemented.

## CMake Integration

### Host (macOS / Linux)

Add microDecoder as a subdirectory or via `FetchContent`, then link against it:

```cmake
add_subdirectory(path/to/micro-decoder)
target_link_libraries(my_app PRIVATE micro_decoder)
```

The library fetches its codec dependencies (`micro-flac`, `micro-mp3`, `micro-opus`, `micro-wav`) automatically via `FetchContent`. Your system must provide `libcurl` and a C++17 compiler.

### ESP-IDF

Place microDecoder under your project's `components/` directory (or declare it as a managed component). The `CMakeLists.txt` detects an ESP-IDF build automatically when `IDF_TARGET` is defined and registers the component:

```cmake
# No additional CMake changes needed -- idf_component_register is called automatically
```

Declare the dependency in your component's `idf_component.yml` or add `micro_decoder` to your main component's `REQUIRES` list:

```cmake
idf_component_register(
    SRCS "main.cpp"
    REQUIRES micro_decoder
)
```

## Codec Selection

All four codecs (FLAC, MP3, Opus, WAV) are enabled by default. Disable individual codecs to reduce binary size.

### Host (CMake options)

Pass `-D` flags when configuring:

```bash
cmake -B build -DMICRO_DECODER_CODEC_OPUS=OFF -DMICRO_DECODER_CODEC_WAV=OFF
```

| Option | Default |
|---|---|
| `MICRO_DECODER_CODEC_FLAC` | `ON` |
| `MICRO_DECODER_CODEC_MP3` | `ON` |
| `MICRO_DECODER_CODEC_OPUS` | `ON` |
| `MICRO_DECODER_CODEC_WAV` | `ON` |

Disabled codecs are not fetched or compiled.

### ESP-IDF (Kconfig)

Configure via menuconfig:

```bash
idf.py menuconfig
# Navigate to: Micro Decoder
```

Each codec has a `CONFIG_MICRO_DECODER_CODEC_*` bool option (default `y`). Disabling a codec removes all of its code paths from microDecoder and excludes the corresponding codec library from the build.

## Headers

Include `micro_decoder/decoder_source.h` to get `DecoderSource`. Include `micro_decoder/types.h` for `DecoderConfig`, `DecoderListener`, `DecoderState`, `AudioStreamInfo`, and `AudioFileType`.

```cpp
#include "micro_decoder/decoder_source.h"
#include "micro_decoder/types.h"
```

All symbols live in `namespace micro_decoder`.

## Step 1: Implement the DecoderListener

Subclass `DecoderListener` and implement all three callbacks. All methods are pure virtual and required.

```cpp
using namespace micro_decoder;

struct MyAudioSink : DecoderListener {
    // REQUIRED: Write decoded PCM audio to the platform audio output.
    // Called from the decoder thread. May block up to timeout_ms waiting for
    // the sink to accept data -- use this for natural backpressure rather than
    // busy-waiting. Return the number of bytes actually written; returning less
    // than length causes the decoder source to retry with the remainder.
    size_t on_audio_write(const uint8_t* data, size_t length, uint32_t timeout_ms) override {
        return my_audio_output.write(data, length, timeout_ms);
    }

    // REQUIRED: Called once when the stream format is known.
    // Use this to configure your audio output before the first on_audio_write() call.
    void on_stream_info(const AudioStreamInfo& info) override {
        my_audio_output.configure(
            info.get_sample_rate(),
            info.get_channels(),
            info.get_bits_per_sample()
        );
    }

    // REQUIRED: Called on state transitions (IDLE, PLAYING, FAILED).
    void on_state_change(DecoderState state) override {
        if (state == DecoderState::FAILED) {
            log_error("Decoding failed");
        }
    }
};
```

### Thread Safety

`on_state_change()` is called exclusively from the thread that calls `DecoderSource::loop()`. It is safe to call `stop()`, `play_url()`, or `play_buffer()` from this callback.

`on_stream_info()` and `on_audio_write()` are called from the decoder thread. Do not call `stop()`, `play_url()`, or `play_buffer()` from these callbacks.

## Step 2: Configure the DecoderSource

`DecoderConfig` has defaults suitable for most use cases. Override fields only when needed.

```cpp
DecoderConfig config;
config.ring_buffer_size    = 500 * 1024;  // 500 KB (increase for high-bitrate streams)
config.transfer_buffer_size = 16 * 1024; // 16 KB (increase from 8 KB default)
config.http_timeout_ms        = 5000;    // HTTP connect/read timeout
config.audio_write_timeout_ms = 50;      // Override default of 25 ms
```

On ESP-IDF, you can also configure task priorities and stack sizes:

```cpp
// ESP-IDF only
config.reader_priority   = 2;
config.decoder_priority  = 2;
config.reader_stack_size = 8192;          // Increase from 5120 default
config.decoder_stack_size = 8 * 1024;    // Override default of 5120
config.decoder_stack_in_psram = false;  // Set true to put the decoder task stack in PSRAM
```

## Step 3: Create the DecoderSource and Wire the Listener

```cpp
MyAudioSink sink;

DecoderSource decoder(config);  // or DecoderSource decoder; for defaults
decoder.set_listener(&sink);    // Must outlive the decoder
```

`DecoderSource` is non-copyable and non-movable. The listener must remain valid until `stop()` returns or the decoder is destroyed.

## Step 4: Play a URL or Buffer

### Streaming from a URL

```cpp
bool ok = decoder.play_url("http://example.com/song.flac");
if (!ok) {
    // Failed to start (e.g., ring buffer allocation failure)
}
```

`play_url()` always calls `stop()` first, then spawns a reader thread (HTTP → ring buffer) and a decoder thread (ring buffer → PCM). Returns `false` if initialization failed (e.g., the constructor could not allocate event flags) or the ring buffer cannot be allocated; format and connection errors are surfaced later via `DecoderState::FAILED`.

Supported schemes: `http://`, `https://`. The audio format is detected from the `Content-Type` response header, falling back to the URL file extension.

For HTTPS, server certificates are verified against the platform default trust store: on ESP-IDF this is the built-in MbedTLS certificate bundle (requires `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`); on host it is the system trust store. To pin a custom CA — for example a private server — set `DecoderConfig::http_ca_certificate` to the PEM-encoded certificate content (not a file path):

```cpp
DecoderConfig config;
config.http_ca_certificate = R"(-----BEGIN CERTIFICATE-----
MIID...
-----END CERTIFICATE-----
)";
```

### Decoding from an In-Memory Buffer

```cpp
std::vector<uint8_t> file_data = load_file("song.mp3");

AudioFileType type = detect_audio_file_type(nullptr, "song.mp3");

bool ok = decoder.play_buffer(file_data.data(), file_data.size(), type);
```

The buffer must remain valid until `stop()` returns or the decoder is destroyed. `play_buffer()` always calls `stop()` first, then spawns only a decoder thread -- no reader thread is needed. Returns `false` if `data` is null, `length` is zero, or `type` is `AudioFileType::NONE` (state unchanged), or if initialization fails (state transitions to `FAILED`). Decode errors are surfaced later via `DecoderState::FAILED`.

### Detecting the Audio Format

`detect_audio_file_type()` takes a Content-Type string (or `nullptr`) and a URL or filename. It checks the Content-Type header first and falls back to the file extension.

```cpp
// From a URL (no Content-Type available yet)
AudioFileType type = detect_audio_file_type(nullptr, "http://example.com/track.mp3");

// From a local file path
AudioFileType type = detect_audio_file_type(nullptr, "/sdcard/track.flac");

// From an HTTP response header
AudioFileType type = detect_audio_file_type("audio/flac", nullptr);
```

Returns `AudioFileType::NONE` if the format cannot be determined.

## Step 5: Pump Events with loop()

Call `decoder.loop()` regularly from your thread. This pumps pending events and fires `on_state_change()` callbacks on the calling thread. `on_stream_info()` and `on_audio_write()` fire directly from the decoder thread for lowest latency.

```cpp
// Wait for decoding to begin
while (decoder.state() == DecoderState::IDLE) {
    decoder.loop();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
// Check for early failure (e.g., connection refused, unsupported format)
if (decoder.state() == DecoderState::FAILED) {
    // Handle error...
}
// Wait for decoding to finish
while (decoder.state() == DecoderState::PLAYING) {
    decoder.loop();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

If you never call `loop()`, `on_state_change()` will not fire. Decoding, `on_stream_info()`, and `on_audio_write()` still work regardless.

## Step 6: Stop Playback

Call `stop()` to abort playback and join all threads. It is safe to call `stop()` at any time, including from `on_state_change()` callbacks fired by `loop()`. If the decoder was `PLAYING` or `FAILED`, the state transitions to `IDLE` (deferred -- the `on_state_change(IDLE)` callback fires on the next `loop()` call). If the decoder was already `IDLE`, the state is unchanged.

```cpp
decoder.stop();  // Blocks until reader and decoder threads have exited
```

The destructor calls `stop()` automatically.

## Logging

Control log verbosity before creating the decoder source:

```cpp
// host builds only; no-op on ESP-IDF
micro_decoder::set_log_level(micro_decoder::LOG_LEVEL_DEBUG);
```

| Constant | Value | Description |
|---|---|---|
| `LOG_LEVEL_ERROR` | 1 | Errors only |
| `LOG_LEVEL_WARN` | 2 | Warnings and above |
| `LOG_LEVEL_INFO` | 3 | Informational and above (default) |
| `LOG_LEVEL_DEBUG` | 4 | All messages |

On ESP-IDF, use the standard `LOG_LOCAL_LEVEL` mechanism instead.

## Minimal Example

A minimal integration that plays a URL and discards the decoded audio:

```cpp
#include "micro_decoder/decoder_source.h"
#include "micro_decoder/types.h"

#include <chrono>
#include <thread>

using namespace micro_decoder;

struct NullSink : DecoderListener {
    void on_stream_info(const AudioStreamInfo&) override {}
    size_t on_audio_write(const uint8_t* data, size_t length, uint32_t /*timeout_ms*/) override {
        return length;  // Discard audio
    }
    void on_state_change(DecoderState) override {}
};

int main() {
    NullSink sink;
    DecoderSource decoder;
    decoder.set_listener(&sink);

    if (!decoder.play_url("http://example.com/song.mp3")) {
        return 1;
    }

    while (decoder.state() == DecoderState::IDLE) {
        decoder.loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool failed = decoder.state() == DecoderState::FAILED;
    if (!failed) {
        while (decoder.state() == DecoderState::PLAYING) {
            decoder.loop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        failed = decoder.state() == DecoderState::FAILED;
    }

    decoder.stop();
    return failed ? 1 : 0;
}
```

## Configuration Reference

### DecoderConfig

| Field | Type | Default | Description |
|---|---|---|---|
| `ring_buffer_size` | `size_t` | `49152` (48 KB) | Ring buffer size in bytes between the reader and decoder threads. Larger values absorb more HTTP jitter at the cost of memory. |
| `transfer_buffer_size` | `size_t` | `8192` (8 KB) | Flat staging buffer size in bytes. Used by the reader to batch HTTP data into the ring buffer and by the decoder for its output buffer. |
| `http_timeout_ms` | `uint32_t` | `5000` | HTTP connect and read timeout in milliseconds. |
| `http_user_agent` | `std::string` | `"micro-decoder/<version> (https://github.com/esphome-libs/micro-decoder)"` | User-Agent header value sent with streaming requests. Set to empty to fall back to the underlying HTTP client's default. |
| `http_ca_certificate` | `std::string` | `""` | PEM-encoded CA certificate(s) used to verify HTTPS servers. Empty falls back to the platform default trust store (MbedTLS certificate bundle on ESP-IDF when `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` is enabled; system trust store on host). Ignored for plain HTTP. |
| `audio_write_timeout_ms` | `uint32_t` | `25` | Maximum time to block in `on_audio_write()` per call, in milliseconds. |
| `reader_write_timeout_ms` | `uint32_t` | `25` | Maximum time the reader blocks writing to the ring buffer per call, in milliseconds. |
| `http_rx_buffer_size` | `size_t` | `2048` | ESP-IDF HTTP client receive buffer size in bytes. ESP-IDF only. |
| `reader_stack_size` | `size_t` | `5120` (5 KB) | Reader task stack size in bytes. ESP-IDF only. |
| `decoder_stack_size` | `size_t` | `5120` (5 KB) | Decoder task stack size in bytes. ESP-IDF only. |
| `reader_priority` | `int` | `2` | FreeRTOS priority for the reader task. ESP-IDF only. |
| `decoder_priority` | `int` | `2` | FreeRTOS priority for the decoder task. ESP-IDF only. |
| `decoder_stack_in_psram` | `bool` | `false` | Allocate the decoder task stack in PSRAM. The reader task stack is always in internal RAM. ESP-IDF only. |

---

## Types Reference

### AudioStreamInfo

Describes the decoded PCM format delivered via `on_audio_write()`.

| Method | Return type | Description |
|---|---|---|
| `get_bits_per_sample()` | `uint8_t` | Bits per sample |
| `get_channels()` | `uint8_t` | Number of channels |
| `get_sample_rate()` | `uint32_t` | Sample rate in Hz |
| `frames_to_bytes(uint32_t)` | `size_t` | Convert frame count to bytes |
| `samples_to_bytes(uint32_t)` | `size_t` | Convert sample count to bytes |

The default `AudioStreamInfo` constructor produces 16-bit, mono, 16000 Hz. The actual stream format is delivered via `on_stream_info()` before the first `on_audio_write()` call.

---

### AudioFileType

| Value | Description |
|---|---|
| `NONE` | Unknown or undetected format (always available) |
| `FLAC` | FLAC (only when `MICRO_DECODER_CODEC_FLAC` is enabled) |
| `MP3` | MP3 (only when `MICRO_DECODER_CODEC_MP3` is enabled) |
| `OPUS` | Opus/OGG (only when `MICRO_DECODER_CODEC_OPUS` is enabled) |
| `WAV` | WAV (only when `MICRO_DECODER_CODEC_WAV` is enabled) |

Use `audio_file_type_to_string(AudioFileType)` to get a human-readable name.

---

### DecoderState

| Value | Description |
|---|---|
| `IDLE` | No active playback; ready |
| `PLAYING` | Decoding and delivering audio |
| `FAILED` | Unrecoverable error (e.g., HTTP failure, unsupported format) |

Transitions fire `on_state_change()` on the thread calling `loop()`. After `FAILED`, call `play_url()` or `play_buffer()` to start a new stream (they call `stop()` internally).
