# Decode Benchmark for ESP32

Measures decoding performance for all four supported audio formats (FLAC, MP3, Opus, WAV) using `DecoderSource::play_buffer()`. Decodes ~10-second audio clips embedded in flash and reports timing statistics. No audio output.

## Features

- Embedded test audio in all four formats (no filesystem or network required)
- Per-codec timing with Real-Time Factor (RTF)
- Runs in a continuous loop for repeated measurements
- Pre-configured for maximum performance (240 MHz, PSRAM, `-O2`)

## Audio Source

Audio source: Beethoven Symphony No. 3 "Eroica", Op. 55, Movement I, Czech National Symphony Orchestra, from the [Musopen Collection](https://musopen.org/) (public domain). 10-second excerpt starting at 1:00, downmixed to 48 kHz mono.

The same source recording is used by the microFLAC, microMP3, and microOpus decode benchmarks.

| Format | Bitrate | File Size |
|--------|---------|-----------|
| FLAC   | lossless | ~779 KB |
| MP3    | 128 kbps | ~157 KB |
| Opus   | 64 kbps  | ~84 KB |
| WAV    | PCM 16-bit | ~938 KB |

## Building and Running

### PlatformIO

```bash
# Build for ESP32-S3
pio run -e esp32s3

# Build and flash for ESP32-S3
pio run -e esp32s3 -t upload -t monitor

# Build for ESP32
pio run -e esp32
```

### ESP-IDF

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

## Expected Output

ESP32-S3 @ 240 MHz:

```api
=== microDecoder Decode Benchmark ===
Decoding ~10s clips, 48 kHz mono, no audio output

--- Iteration 1 ---
  Codec   File size     Decode time  Duration   RTF
  FLAC    798147 bytes     341.0 ms  10.00 s audio  RTF 0.0341
  MP3     161256 bytes     479.4 ms  10.06 s audio  RTF 0.0477
  OPUS     85663 bytes     799.6 ms  10.00 s audio  RTF 0.0800
  WAV     960264 bytes      69.1 ms  10.00 s audio  RTF 0.0069
```

RTF (Real-Time Factor) = decode_time / audio_duration. Lower is better:

- RTF < 1.0: decoding is faster than real-time
- RTF = 1.0: decoding keeps up with real-time playback
- RTF > 1.0: decoding is slower than real-time

## Regenerating Test Audio

To regenerate the embedded audio headers from a different source file:

1. Download the source FLAC from [Musopen](https://musopen.org/) or [Archive.org](https://archive.org/)
2. Run the conversion script:

```bash
python3 convert_audio.py -i path/to/eroica.flac -s 60 -d 10
```

This extracts a 10-second clip starting at 1:00, encodes to all four formats (48 kHz mono), and generates C headers in `src/`.

## File Structure

```api
decode_benchmark/
├── src/
│   ├── CMakeLists.txt          # ESP-IDF component file
│   ├── main.cpp                # Benchmark code
│   ├── test_audio_flac.h       # Embedded FLAC data
│   ├── test_audio_mp3.h        # Embedded MP3 data
│   ├── test_audio_opus.h       # Embedded Opus data
│   └── test_audio_wav.h        # Embedded WAV data
├── CMakeLists.txt              # ESP-IDF project file
├── convert_audio.py            # Audio-to-header conversion script
├── partitions_4mb.csv          # Custom partition table
├── platformio.ini              # PlatformIO configuration
├── sdkconfig.defaults          # Shared ESP-IDF configuration
├── sdkconfig.defaults.esp32    # ESP32 PSRAM and flash settings
├── sdkconfig.defaults.esp32s3  # ESP32-S3 PSRAM, flash, and cache settings
└── README.md
```
