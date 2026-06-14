#!/usr/bin/env python3
"""
Convert an audio file to C header files for all four codecs.

Extracts a clip, converts to 48 kHz mono, and encodes to FLAC, MP3, Opus, and WAV.
Each encoded file is then converted to a C header with an embedded byte array.

Requirements:
    - ffmpeg (command-line tool)
    - Python 3.9+

Usage:
    python convert_audio.py -i input.flac
    python convert_audio.py -i input.flac -s 60 -d 10
"""

import argparse
import os
import subprocess
import sys
import tempfile


CODECS = [
    {
        "name": "flac",
        "ext": "flac",
        "variable": "test_audio_flac_data",
        "ffmpeg_args": ["-c:a", "flac"],
    },
    {
        "name": "mp3",
        "ext": "mp3",
        "variable": "test_audio_mp3_data",
        "ffmpeg_args": ["-c:a", "libmp3lame", "-b:a", "128k"],
    },
    {
        "name": "opus",
        "ext": "ogg",
        "variable": "test_audio_opus_data",
        "ffmpeg_args": ["-c:a", "libopus", "-b:a", "64k"],
    },
    {
        "name": "wav",
        "ext": "wav",
        "variable": "test_audio_wav_data",
        "ffmpeg_args": ["-c:a", "pcm_s16le"],
    },
]


def encode_audio(input_path: str, output_path: str, start: float, duration: float,
                 ffmpeg_args: list[str]) -> None:
    """Encode a clip from the input file using ffmpeg."""
    cmd = [
        "ffmpeg", "-y",
        "-ss", str(start),
        "-t", str(duration),
        "-i", input_path,
        "-ac", "1",
        "-ar", "48000",
        *ffmpeg_args,
        output_path,
    ]
    subprocess.run(cmd, check=True, capture_output=True)


def binary_to_header(input_path: str, output_path: str, variable_name: str) -> None:
    """Convert a binary file to a C header with an embedded byte array."""
    with open(input_path, "rb") as f:
        data = f.read()

    file_size = len(data)
    header_guard = variable_name.upper() + "_H"

    with open(output_path, "w") as f:
        f.write(f"// Auto-generated from {os.path.basename(input_path)}\n")
        f.write(f"// Size: {file_size} bytes\n\n")
        f.write(f"#ifndef {header_guard}\n")
        f.write(f"#define {header_guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"const uint32_t {variable_name}_len = {file_size};\n\n")
        f.write(f"const uint8_t {variable_name}[] = {{\n")

        bytes_per_line = 16
        for i in range(0, file_size, bytes_per_line):
            chunk = data[i : i + bytes_per_line]
            hex_values = ", ".join(f"0x{b:02x}" for b in chunk)

            if i + bytes_per_line < file_size:
                f.write(f"    {hex_values},\n")
            else:
                f.write(f"    {hex_values}\n")

        f.write("};\n\n")
        f.write(f"#endif // {header_guard}\n")

    print(f"  {output_path}: {file_size} bytes ({file_size / 1024:.1f} KB)")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert audio to C headers for all four codecs"
    )
    parser.add_argument("-i", "--input", required=True, help="Input audio file")
    parser.add_argument(
        "-s", "--start", type=float, default=60.0,
        help="Start time in seconds (default: 60)"
    )
    parser.add_argument(
        "-d", "--duration", type=float, default=10.0,
        help="Clip duration in seconds (default: 10)"
    )
    parser.add_argument(
        "-o", "--output-dir", default="src",
        help="Output directory for headers (default: src)"
    )

    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"Error: Input file '{args.input}' not found", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.output_dir, exist_ok=True)

    print(f"Input: {args.input}")
    print(f"Clip: {args.start}s to {args.start + args.duration}s ({args.duration}s)")
    print(f"Output: 48 kHz mono, 16-bit\n")

    with tempfile.TemporaryDirectory() as tmpdir:
        for codec in CODECS:
            encoded_path = os.path.join(tmpdir, f"clip.{codec['ext']}")
            header_path = os.path.join(args.output_dir, f"test_audio_{codec['name']}.h")

            print(f"Encoding {codec['name'].upper()}...")
            encode_audio(args.input, encoded_path, args.start, args.duration,
                         codec["ffmpeg_args"])
            binary_to_header(encoded_path, header_path, codec["variable"])

    print("\nDone!")


if __name__ == "__main__":
    main()
