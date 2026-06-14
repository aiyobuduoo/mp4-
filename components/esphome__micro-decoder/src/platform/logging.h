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

/// @file logging.h
/// @brief Platform-abstracted logging macros mapping MD_LOG* to ESP-IDF or fprintf-based output

#pragma once

#ifdef ESP_PLATFORM

#include <esp_log.h>

#define MD_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define MD_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define MD_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define MD_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)

namespace micro_decoder {

/// @brief No-op on ESP-IDF; log levels are controlled at compile time
/// @param level Log level (ignored; ESP-IDF log levels are set at compile time)
inline void platform_set_log_level(int /*level*/) {}

}  // namespace micro_decoder

#else  // Host

#include <atomic>
#include <cstdio>

#define MD_LOG_ERROR 1
#define MD_LOG_WARN 2
#define MD_LOG_INFO 3
#define MD_LOG_DEBUG 4

namespace micro_decoder {
inline std::atomic<int> md_host_log_level{MD_LOG_INFO};
}  // namespace micro_decoder

// clang-format off
#define MD_LOGE(tag, fmt, ...) do { if (micro_decoder::md_host_log_level.load(std::memory_order_relaxed) >= MD_LOG_ERROR)   fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__); } while(0)
#define MD_LOGW(tag, fmt, ...) do { if (micro_decoder::md_host_log_level.load(std::memory_order_relaxed) >= MD_LOG_WARN)    fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__); } while(0)
#define MD_LOGI(tag, fmt, ...) do { if (micro_decoder::md_host_log_level.load(std::memory_order_relaxed) >= MD_LOG_INFO)    fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__); } while(0)
#define MD_LOGD(tag, fmt, ...) do { if (micro_decoder::md_host_log_level.load(std::memory_order_relaxed) >= MD_LOG_DEBUG)   fprintf(stderr, "D %s: " fmt "\n", tag, ##__VA_ARGS__); } while(0)
// clang-format on

namespace micro_decoder {

/// @brief Sets the runtime log level
/// @param level One of the MD_LOG_* constants
inline void platform_set_log_level(int level) {
    micro_decoder::md_host_log_level.store(level, std::memory_order_relaxed);
}

}  // namespace micro_decoder

#endif  // ESP_PLATFORM
