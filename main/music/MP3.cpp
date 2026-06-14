#include "MP3.h"

#include <dirent.h>
#include <inttypes.h>
#include <memory>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "micro_decoder/decoder_source.h"
#include "micro_decoder/types.h"

extern "C" {
#include "es8311_code.h"
}

static const char *TAG = "MP3_PLAYER";

namespace {

constexpr size_t MP3_PATH_MAX_LEN = 256;
static portMUX_TYPE s_progress_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_position_ms;
static uint32_t s_bytes_per_second;
static uint32_t s_position_remainder;
static bool s_playback_active;

static void mp3_progress_reset(uint32_t initial_position_ms = 0)
{
    portENTER_CRITICAL(&s_progress_mux);
    s_position_ms = initial_position_ms;
    s_bytes_per_second = 0;
    s_position_remainder = 0;
    s_playback_active = false;
    portEXIT_CRITICAL(&s_progress_mux);
}

enum Mp3CommandType {
    MP3_CMD_PLAY_URL = 0,
    MP3_CMD_PLAY_FILE,
    MP3_CMD_PLAY_FILE_OFFSET,
    MP3_CMD_STOP,
};

struct Mp3Command {
    Mp3CommandType type;
    char path[MP3_PATH_MAX_LEN];
    size_t offset;
    uint32_t position_ms;
    TaskHandle_t completion_task;
};

class ES8311DecoderListener : public micro_decoder::DecoderListener {
public:
    void on_stream_info(const micro_decoder::AudioStreamInfo &info) override
    {
        ESP_LOGI(TAG, "Audio stream: sample_rate=%" PRIu32 ", channels=%u, bits=%u",
                 info.get_sample_rate(), info.get_channels(), info.get_bits_per_sample());
        esp_err_t err = es8311_codec_set_stream_format(
            info.get_sample_rate(),
            info.get_channels(),
            info.get_bits_per_sample());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reconfigure ES8311 stream: %s", esp_err_to_name(err));
        }
        portENTER_CRITICAL(&s_progress_mux);
        s_bytes_per_second = info.frames_to_bytes(info.get_sample_rate());
        portEXIT_CRITICAL(&s_progress_mux);
    }

    size_t on_audio_write(const uint8_t *data, size_t length, uint32_t timeout_ms) override
    {
        size_t written = 0;
        esp_err_t ret = es8311_codec_write(data, length, &written, timeout_ms);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ES8311 write failed: %s", esp_err_to_name(ret));
            return 0;
        }
        portENTER_CRITICAL(&s_progress_mux);
        if (s_bytes_per_second != 0) {
            uint64_t numerator = (uint64_t)written * 1000U + s_position_remainder;
            s_position_ms += (uint32_t)(numerator / s_bytes_per_second);
            s_position_remainder = (uint32_t)(numerator % s_bytes_per_second);
        }
        portEXIT_CRITICAL(&s_progress_mux);
        return written;
    }

    void on_state_change(micro_decoder::DecoderState state) override
    {
        ESP_LOGI(TAG, "Decoder state changed: %d", static_cast<int>(state));
        portENTER_CRITICAL(&s_progress_mux);
        s_playback_active = state == micro_decoder::DecoderState::PLAYING;
        portEXIT_CRITICAL(&s_progress_mux);
    }
};

static ES8311DecoderListener s_listener;
static std::unique_ptr<micro_decoder::DecoderSource> s_decoder;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_decoder_task_handle;

static void mp3_handle_command(const Mp3Command &cmd)
{
    switch (cmd.type) {
    case MP3_CMD_PLAY_URL:
        mp3_progress_reset();
        ESP_LOGI(TAG, "Start playing URL: %s", cmd.path);
        if (!s_decoder->play_url(cmd.path)) {
            ESP_LOGE(TAG, "play_url failed");
        }
        break;
    case MP3_CMD_PLAY_FILE: {
        mp3_progress_reset();
        micro_decoder::AudioFileType file_type =
            micro_decoder::detect_audio_file_type(nullptr, cmd.path);
        if (file_type == micro_decoder::AudioFileType::NONE) {
            ESP_LOGE(TAG, "Unsupported audio file type: %s", cmd.path);
            break;
        }

        ESP_LOGI(TAG, "Start playing file: %s (%s)", cmd.path,
                 micro_decoder::audio_file_type_to_string(file_type));
        if (!s_decoder->play_file(cmd.path)) {
            ESP_LOGE(TAG, "play_file failed");
        }
        break;
    }
    case MP3_CMD_PLAY_FILE_OFFSET: {
        mp3_progress_reset(cmd.position_ms);
        ESP_LOGI(TAG, "Seek file: %s, offset=%u, position=%" PRIu32 " ms",
                 cmd.path, (unsigned)cmd.offset, cmd.position_ms);
        if (!s_decoder->play_file_from_offset(cmd.path, cmd.offset)) {
            ESP_LOGE(TAG, "play_file_from_offset failed");
        }
        break;
    }
    case MP3_CMD_STOP:
        ESP_LOGI(TAG, "Stop playback");
        s_decoder->stop();
        break;
    default:
        ESP_LOGW(TAG, "Unknown MP3 command: %d", static_cast<int>(cmd.type));
        break;
    }
}

static void mp3_decoder_task(void *arg)
{
    ESP_LOGI(TAG, "MP3 decoder task started");

    Mp3Command cmd = {};
    while (true) {
        if (xQueueReceive(s_command_queue, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
            mp3_handle_command(cmd);
            if (cmd.completion_task != nullptr) {
                xTaskNotifyGive(cmd.completion_task);
            }
        }

        s_decoder->loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

extern "C" void mp3_init(void)
{
    if (s_decoder) {
        return;
    }

    micro_decoder::DecoderConfig config = {};
    config.audio_write_timeout_ms = 10;
    config.reader_write_timeout_ms = 10;
    config.reader_priority = 1;
    config.decoder_priority = 1;
    config.decoder_stack_size = 6144;
    config.reader_stack_size = 6144;

    s_decoder = std::make_unique<micro_decoder::DecoderSource>(config);
    s_decoder->set_listener(&s_listener);

    s_command_queue = xQueueCreate(4, sizeof(Mp3Command));
    if (s_command_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create MP3 command queue");
        s_decoder.reset();
        return;
    }

    if (xTaskCreatePinnedToCoreWithCaps(
            mp3_decoder_task,
            "mp3_decoder",
            8192,
            nullptr,
            3,
            &s_decoder_task_handle,
            1,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MP3 decoder task");
        vQueueDelete(s_command_queue);
        s_command_queue = nullptr;
        s_decoder.reset();
        return;
    }

    ESP_LOGI(TAG, "MP3 player initialized, task pinned to core 1 with PSRAM stack");
}

extern "C" void mp3_play_url(const char *url)
{
    if (url == nullptr || url[0] == '\0') {
        ESP_LOGE(TAG, "URL is empty");
        return;
    }
    if (s_command_queue == nullptr) {
        ESP_LOGE(TAG, "MP3 player is not initialized");
        return;
    }

    Mp3Command cmd = {};
    cmd.type = MP3_CMD_PLAY_URL;
    strncpy(cmd.path, url, sizeof(cmd.path) - 1);
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    if (xQueueSend(s_command_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to enqueue play command");
    }
}

extern "C" void mp3_play_file(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        ESP_LOGE(TAG, "File path is empty");
        return;
    }
    if (s_command_queue == nullptr) {
        ESP_LOGE(TAG, "MP3 player is not initialized");
        return;
    }

    Mp3Command cmd = {};
    cmd.type = MP3_CMD_PLAY_FILE;
    strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    cmd.path[sizeof(cmd.path) - 1] = '\0';

    if (xQueueSend(s_command_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to enqueue file play command");
    }
}

extern "C" void mp3_play_file_from_offset(const char *path, size_t offset, uint32_t position_ms)
{
    if (path == nullptr || path[0] == '\0' || s_command_queue == nullptr) return;
    Mp3Command cmd = {};
    cmd.type = MP3_CMD_PLAY_FILE_OFFSET;
    cmd.offset = offset;
    cmd.position_ms = position_ms;
    strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    if (xQueueSend(s_command_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to enqueue seek command");
    }
}

extern "C" void mp3_play_first_file_in_dir(const char *dir_path)
{
    if (dir_path == nullptr || dir_path[0] == '\0') {
        ESP_LOGE(TAG, "Directory path is empty");
        return;
    }

    DIR *dir = opendir(dir_path);
    if (dir == nullptr) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return;
    }

    struct dirent *entry = nullptr;
    char path[MP3_PATH_MAX_LEN];
    bool found = false;

    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        int written = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        if (written <= 0 || written >= (int)sizeof(path)) {
            continue;
        }

        micro_decoder::AudioFileType type = micro_decoder::detect_audio_file_type(nullptr, path);
        if (type == micro_decoder::AudioFileType::NONE) {
            continue;
        }

        ESP_LOGI(TAG, "Found audio file in directory: %s", path);
        mp3_play_file(path);
        found = true;
        break;
    }

    closedir(dir);

    if (!found) {
        ESP_LOGW(TAG, "No supported audio file found in: %s", dir_path);
    }
}

extern "C" void mp3_stop(void)
{
    if (s_command_queue == nullptr) {
        return;
    }

    Mp3Command cmd = {};
    cmd.type = MP3_CMD_STOP;
    if (xQueueSend(s_command_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to enqueue stop command");
    }
}

extern "C" bool mp3_stop_and_wait(uint32_t timeout_ms)
{
    if (s_command_queue == nullptr) {
        return false;
    }

    while (ulTaskNotifyTake(pdTRUE, 0) != 0) {
    }
    Mp3Command cmd = {};
    cmd.type = MP3_CMD_STOP;
    cmd.completion_task = xTaskGetCurrentTaskHandle();
    if (xQueueSend(s_command_queue, &cmd, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) != 0;
}

extern "C" uint32_t mp3_get_position_ms(void)
{
    portENTER_CRITICAL(&s_progress_mux);
    uint32_t position = s_position_ms;
    portEXIT_CRITICAL(&s_progress_mux);
    return position;
}

extern "C" bool mp3_is_playing(void)
{
    portENTER_CRITICAL(&s_progress_mux);
    bool active = s_playback_active;
    portEXIT_CRITICAL(&s_progress_mux);
    return active;
}
